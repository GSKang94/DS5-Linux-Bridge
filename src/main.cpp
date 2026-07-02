//
// Created by awalol on 2026/3/4.
//

#include "audio.h"
#include "bsp/board_api.h"
#include "bt.h"
#include "hardware/clocks.h"
#include "hardware/vreg.h"
#include "hardware/watchdog.h"
#include "pico/cyw43_arch.h"
#include "state_mgr.h"
#include "usb.h"
#include "utils.h"
#include "wake.h"
#include <cstdio>

#include "config.h"
#include "dse.h"
#include "wifi_net.h"

#if defined(ENABLE_WIFI_WOL)
#include "bootsel_button.h" // BOOTSEL-held-at-boot -> force WiFi onboarding (AP)
#endif

#if ENABLE_BATT_LED
#include "battery_led.h"
#endif

// Pico SDK speciifically for waiting on conditions
#include "pico/critical_section.h"
#include "pico/time.h"

int reportSeqCounter = 0;
uint8_t packetCounter = 0;
bool spk_active = false;
bool mic_active = false;

uint8_t interrupt_in_data[63] = {
    0x7f, 0x7d, 0x7f, 0x7e, 0x00, 0x00, 0xa7, 0x08, 0x00, 0x00, 0x00,
    0x52, 0x43, 0x30, 0x41, 0x01, 0x00, 0x0e, 0x00, 0xef, 0xff, 0x03,
    0x03, 0x7b, 0x1b, 0x18, 0xf0, 0xcc, 0x9c, 0x60, 0x00, 0xfc, 0x80,
    0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x00, 0x09, 0x09, 0x00,
    0x00, 0x00, 0x00, 0x00, 0xa7, 0xad, 0x60, 0x00, 0x29, 0x18, 0x00,
    0x53, 0x9f, 0x28, 0x35, 0xa5, 0xa8, 0x0c, 0x8b};

critical_section_t report_cs;
volatile bool report_dirty = false;

void interrupt_loop() {
#ifdef ENABLE_WAKE_HID
  // Only the FULL variant exposes the real gamepad (HID instance 0). In MINIMAL
  // instance 0 is an inert dummy HID, so don't emit gamepad reports there.
  // (The keyboard is instance 1 in BOTH variants, so a gamepad report can never
  // reach it regardless -- see usb_descriptors.cpp. This guard just avoids
  // pushing reports at the dummy / before the controller is connected.)
  if (!usb_descriptor_variant_is_full())
    return;
#endif
  if (!tud_hid_ready())
    return;

  // TODO: Refactor for better code reuse
  if (get_config().polling_rate_mode != 2) {
    if (!tud_hid_report(0x01, interrupt_in_data, 63)) {
      printf("[USBHID] tud_hid_report error\n");
    }
    return;
  }

  bool should_send = false;
  // Local buffer to hold the report data while we prepare it to send.
  uint8_t safe_report[63];

  critical_section_enter_blocking(&report_cs);
  if (report_dirty) {
    memcpy(safe_report, interrupt_in_data, 63);
    report_dirty = false;
    should_send = true;
  }
  critical_section_exit(&report_cs);

  // Only send to TinyUSB if we actually grabbed fresh data
  if (should_send) {
    if (!tud_hid_report(0x01, safe_report, 63)) {
      printf("[USBHID] tud_hid_report error\n");

      // If the report failed to queue, restore the dirty flag
      // so we try again on the next loop iteration.
      critical_section_enter_blocking(&report_cs);
      report_dirty = true;
      critical_section_exit(&report_cs);
    }
  }
}

void state_push_to_bt() {
  if (spk_active) {
    return;
  }
  uint8_t outputData[78]{};
  outputData[0] = 0x31;
  outputData[1] = reportSeqCounter << 4;
  if (++reportSeqCounter == 256) {
    reportSeqCounter = 0;
  }
  outputData[2] = 0x10;
  state_get(outputData + 3, sizeof(SetStateData));
  bt_write(outputData, sizeof(outputData));
}

void on_bt_data(CHANNEL_TYPE channel, uint8_t *data, uint16_t len) {
  // printf("[Main] BT data callback: channel=%u len=%u\n", channel, len);
  if (channel == INTERRUPT && len > 2 && data[1] == 0x31) {
    if (data[2] >> 1 & 1) {
      mic_add_queue(data + 4);
      return;
    }

    // Mute button detection (data[12] corresponds to byte 9 of input data)
    if (!g_host_hid_manages_mute) {
      static bool prev_mute_pressed = false;
      bool mute_pressed = (data[12] & 0x04) != 0;
      if (mute_pressed && !prev_mute_pressed) {
        g_firmware_mic_muted = !g_firmware_mic_muted;
        state_set_local_mute(g_firmware_mic_muted);
        state_push_to_bt();
      }
      prev_mute_pressed = mute_pressed;
    }

    // Track actual DS5 jack state separately — interrupt_in_data[53]
    // has its HP_DETECT bit forced high for host UCM routing and cannot
    // be used as the previous-state comparison here.
    static uint8_t last_jack_state =
        0xFF; // sentinel: force set_headset on first report
    const uint8_t cur_jack_state = data[56] & 1;
    if (cur_jack_state != last_jack_state) {
      set_headset(cur_jack_state);
      last_jack_state = cur_jack_state;
    }

    // Wake-on-PS must observe every BT input report regardless of polling
    // mode: the wake feature has its own state to maintain (button-byte
    // diff for edge detection) and short-circuiting it on non-2 polling
    // modes silently breaks wake while the host is suspended.
    // Wake path: USB remote-wakeup (wake.cpp) AND, on ENABLE_WIFI_WOL builds, a
    // companion Wake-on-LAN packet fired from the same request_host_wake()
    // chokepoint inside wake.cpp. Both run when the host is suspended -- USB wake
    // covers S3, WOL covers S4/S5 (indistinguishable over USB; see wake.cpp).
    wake_on_bt_input(data + 3, len - 3);

    // interrupt_in_data[53] = dualsense_input_report.status[1]:
    //   bit 0 = HP_DETECT  (headphones plugged into DS5 3.5mm jack)
    //   bit 1 = MIC_DETECT (headset mic plugged into DS5 3.5mm jack)
    // hid-playstation (≥6.18) reads these and emits SW_HEADPHONE_INSERT /
    // SW_MICROPHONE_INSERT input events. The USB audio mixer quirk (≥6.17)
    // wires those to "Headphone Jack" / "Headset Mic Jack" ALSA controls,
    // which alsa-ucm-conf uses to switch between mono Internal Speaker and
    // stereo Headphones profiles. We pass the DS5's real values through
    // unchanged — the DS5 hardware jack sensor is authoritative.
    if (get_config().polling_rate_mode != 2) {
      memcpy(interrupt_in_data, data + 3, 63);
#if ENABLE_BATT_LED
      battery_led_note_report();
#endif
      return;
    }

    // We add the critical section here to avoid any race conditions when
    // writing to the interrupt_in_data buffer, which is shared between the main
    // loop and this callback. The critical section ensures that only one thread
    // can access the buffer at a time, preventing data corruption and ensuring
    // thread safety. We also set the report_dirty flag to true to indicate that
    // new data is available
    //  and needs to be sent in the next interrupt report.
    critical_section_enter_blocking(&report_cs);
    memcpy(interrupt_in_data, data + 3, 63);
    report_dirty = true;
    critical_section_exit(&report_cs);
#if ENABLE_BATT_LED
    battery_led_note_report();
#endif
  }
}

// Invoked when received GET_REPORT control request
// Application must fill buffer report's content and return its length.
// Return zero will cause the stack to STALL request
uint16_t tud_hid_get_report_cb(uint8_t itf, uint8_t report_id,
                               hid_report_type_t report_type, uint8_t *buffer,
                               uint16_t reqlen) {
#ifdef WAKE_VIA_USB_KBD
  if (itf == usb_kbd_hid_instance()) {
    if (reqlen >= 8) {
      memset(buffer, 0, 8);
      return 8;
    }
    return 0;
  }
#endif
#ifdef ENABLE_WAKE_HID
  // MINIMAL instance 0 is the inert dummy HID, NOT the gamepad. Don't route its
  // GET_REPORT into the BT feature path (which would query a controller that
  // isn't connected). Return 0 (STALL); the host never reads it.
  if (!usb_descriptor_variant_is_full()) {
    return 0;
  }
#endif
  (void)itf;
  (void)report_id;
  (void)report_type;
  (void)buffer;
  (void)reqlen;

  // DSE profiles: while the unlock + prefetch is still in progress, return 0
  // (NAK) for profile reads so the PS app retries rather than caching an
  // empty snapshot. Still kick off the background BT fetch.
  if (dse_is_profile_report(report_id) && !dse_profiles_ready()) {
    get_feature_data(report_id, reqlen);
    return 0;
  }

  std::vector<uint8_t> feature_data = get_feature_data(report_id, reqlen);
  if (!feature_data.empty()) {
    memcpy(buffer, feature_data.data() + 1, feature_data.size() - 1);
  }

  return feature_data.empty() ? 0 : feature_data.size() - 1;
}

bool tud_audio_set_itf_cb(uint8_t rhport,
                          tusb_control_request_t const *p_request) {
  (void)rhport;
  uint8_t const itf = tu_u16_low(p_request->wIndex); // wInterface
  uint8_t const alt = tu_u16_low(p_request->wValue); // bAlternateSetting

  if (itf == 1) {
    printf("[AUDIO] Set interface Speaker to alternate setting %d\n", alt);
    spk_active = alt;
  } else if (itf == 2) {
    printf("[AUDIO] Set interface Mic to alternate setting %d\n", alt);
    mic_active = alt;
  }

  return true;
}

// Invoked when received SET_REPORT control request or
// received data on OUT endpoint ( Report ID = 0, Type = 0 )
void tud_hid_set_report_cb(uint8_t itf, uint8_t report_id,
                           hid_report_type_t report_type, uint8_t const *buffer,
                           uint16_t bufsize) {
#ifdef WAKE_VIA_USB_KBD
  if (itf == usb_kbd_hid_instance()) {
    // Drop keyboard SET_REPORT (host LED state).
    return;
  }
#endif
#ifdef ENABLE_WAKE_HID
  // MINIMAL instance 0 is the inert dummy HID; ignore any report to it.
  if (!usb_descriptor_variant_is_full()) {
    return;
  }
#endif
  (void)itf;
  (void)report_id;
  (void)report_type;
  (void)buffer;
  (void)bufsize;

  // INTERRUPT OUT
  if (report_id == 0) {
    switch (buffer[0]) {
    case 0x02: {
      state_update(buffer + 1, bufsize - 1);
      // When the headset/speaker is active, output reports normally piggyback
      // on the audio frame path, so we defer (break) here to avoid double-send.
      // But a rumble-bearing SetStateData (UseRumbleNotHaptics flags set) must
      // go out NOW, or rumble lags/drops a frame while audio is streaming.
      // (Ported from upstream awalol/DS5Dongle 07ecbb3, issue #182.)
      bool send_now = ((buffer[1] >> 1) & 1) ||  // UseRumbleNotHaptics
                      ((buffer[39] >> 3) & 1);   // UseRumbleNotHaptics2
      if (!send_now && spk_active) {
        break;
      }
      uint8_t outputData[78]{};
      outputData[0] = 0x31;
      outputData[1] = reportSeqCounter << 4;
      if (++reportSeqCounter == 256) {
        reportSeqCounter = 0;
      }
      outputData[2] = 0x10;
      // memcpy(outputData + 3, buffer + 1, bufsize - 1);
      state_get(outputData + 3, sizeof(SetStateData));
      bt_write(outputData, sizeof(outputData));
      break;
    }
    }
  }
  if (report_id == 0x80 ||
      // DSE: Write Profile Block
      report_id == 0x60 || report_id == 0x62 || report_id == 0x61) {
    set_feature_data(report_id, const_cast<uint8_t *>(buffer), bufsize);
    return;
  }
}

int main() {
#if SYS_CLOCK_KHZ != 150000
  // Overclock path: raise core voltage before bumping the system clock.
  // (1.20V is stable/safe for 320 MHz.) At the stock 150 MHz this is skipped —
  // RAM-relocated hot paths make the overclock unnecessary, and the SDK's
  // default clock init handles the stock case.
  vreg_set_voltage(VREG_VOLTAGE_1_20);
  sleep_ms(1000);
  set_sys_clock_khz(SYS_CLOCK_KHZ, true);
#endif

  board_init();
  tusb_rhport_init_t dev_init = {.role = TUSB_ROLE_DEVICE,
                                 .speed = TUSB_SPEED_FULL};
  tusb_init(BOARD_TUD_RHPORT, &dev_init);
  sleep_ms(150);
  tud_disconnect();
  board_init_after_tusb();

#if defined(ENABLE_WIFI_WOL) && defined(WIFI_BOOTSEL_REONBOARD)
  // OPTIONAL WiFi re-onboard trigger: hold BOOTSEL during the first ~2s of boot
  // to force AP + captive portal even when creds are saved. DISABLED BY DEFAULT
  // (WIFI_BOOTSEL_REONBOARD undefined) because the RP2350 BOOTSEL read in
  // bootsel_button.h was observed to FALSE-TRIGGER on every boot -- it reported
  // "held" with nothing pressed, stranding the device in AP mode forever. The
  // logic-based fallback below (provisioned -> STA; if the join fails within the
  // budget, wifi_net_task() clears creds + reboots to AP) makes this unnecessary.
  // DO NOT re-enable until bootsel_button_pressed() is fixed + verified on HW.
  // Sampled before cyw43_arch_init/BT/audio.
  {
    bool held = true;
    for (int i = 0; i < 20 && held; i++) { // ~2s @ 100ms
      held = bootsel_button_pressed();
      sleep_ms(100);
    }
    if (held) {
      printf("[BOOT] BOOTSEL held -> forcing WiFi onboarding (AP mode)\n");
      wifi_net_request_ap_onboarding();
    }
  }
#endif

  if (cyw43_arch_init()) {
    printf("Failed to initialize CYW43\n");
    return 1;
  }

  // Load persisted config from flash BEFORE wifi_net_init(): it reads the
  // stored WiFi credentials (STA vs AP onboarding) and the mDNS hostname, so
  // the saved values must be in place first.
  config_load();

  // Bring up the config web server (no-op with ENABLE_WIFI_WOL off). The SDK's
  // cyw43_arch_init() already brought lwIP up (CYW43_LWIP=1), so
  // wifi_net_init() must NOT re-init it. Diagnostics print to UART0 (GP0 TX,
  // 115200 8N1), not USB.
  wifi_net_init();  // Wi-Fi/WOL transport (ENABLE_WIFI_WOL)

  // Power-On Self Test (POST) LED pattern: 3 rapid flashes to confirm
  // successful CPU overclocking and CYW43 Bluetooth module initialization.
  for (int i = 0; i < 6; i++) {
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, i % 2 == 0);
    sleep_ms(80);
  }
  cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, false);

#if ENABLE_BATT_LED
  battery_led_init();
#endif

  if (watchdog_caused_reboot()) {
    printf("Rebooted by Watchdog!\n");
    // 当崩溃重启以后，闪三下灯
    for (int i = 0; i < 6; i++) {
      if (i % 2 == 0) {
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, true);
      } else {
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, false);
      }
      sleep_ms(500);
    }
  } else {
    printf("Clean boot\n");
  }

  // Initialize the critical section for the report buffer
  critical_section_init(&report_cs);
  wake_init();

  // WiFi onboarding (AP + captive portal) is a dedicated setup mode: no
  // controller, no audio. Crucially, BT classic page-scan/inquiry contends with
  // the SoftAP on the single shared CYW43 radio -- with BT up, the AP beacons
  // but never admits a station (observed: stas=0, client loops DHCP forever).
  // So in AP mode we skip BT + audio entirely, handing the radio to the AP (and
  // freeing ~110 KB of heap). Normal STA operation brings BT/audio up as usual.
  const bool ap_onboarding = wifi_net_in_ap_mode();
  if (!ap_onboarding) {
    bt_init();
    bt_register_data_callback(on_bt_data);

    audio_init();
    state_init();
  } else {
    printf("[BOOT] AP onboarding mode: skipping BT + audio (radio handed to SoftAP)\n");
  }

#ifdef ENABLE_WAKE_HID
  // Enumerate immediately as the MINIMAL variant (inert HID placeholder, plus
  // the boot keyboard in WAKE_VIA_USB_KBD builds), even before any controller
  // connects. tusb_init() left us tud_disconnect()'d; without this the dongle
  // would stay invisible to the host on a cold plug-in until the first
  // controller connection flipped it to FULL -- and the device must be
  // enumerated before the host suspends for USB remote-wakeup to work.
  // active_variant/desired_variant are already MINIMAL.
  tud_connect();
#endif

  watchdog_enable(1000, true);

  // Onboarding loop: a stripped main loop with BT/audio/HID skipped (they were
  // never initialised in AP mode). Pump only the radio/lwIP (cyw43_arch_poll +
  // wifi_net_task drive the SoftAP RX, DHCP/DNS servers, scan, captive portal)
  // plus tud_task to keep USB alive, and feed the watchdog. The device leaves
  // this loop by rebooting into STA mode once the user provisions (wifi_net_task
  // fires the deferred watchdog_reboot).
  if (ap_onboarding) {
    while (1) {
      watchdog_update();
      cyw43_arch_poll();
      tud_task();
      wifi_net_task();
      sleep_us(250);
    }
  }

  while (1) {
    watchdog_update();
    cyw43_arch_poll();
    bt_connection_watchdog_tick();
    bt_blacklist_persist_if_dirty();
    bt_pump();
    tud_task();
    wake_task();
#ifdef ENABLE_WAKE_HID
    usb_variant_task();
#endif
    // Service lwIP for the config web server (no-op unless the WiFi transport
    // is built). Cheap; not in the audio hot path. wifi_net_task pumps the
    // WiFi link state + lwIP timers.
    wifi_net_task();
    audio_loop();
    interrupt_loop();
    // DSE Edge profile snapshot prefetch/unlock state machine.
    dse_task();
#if ENABLE_BATT_LED
    battery_led_tick();
#endif
    // Yield only when the hot paths are idle; otherwise keep draining USB/BT.
    if (!tud_audio_available() && !bt_send_pending()) {
      sleep_us(250);
    } else {
      tight_loop_contents();
    }
  }
}
