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

#if ENABLE_SERIAL
#include "pico/stdio_usb.h"
#endif
#include "cmd.h"
#include "config.h"

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
#if ENABLE_DIAG
static volatile uint32_t main_loop_gap_max_us = 0;
static volatile uint32_t cyw43_poll_max_us = 0;
static volatile uint32_t tud_task_max_us = 0;
static volatile uint32_t audio_loop_max_us = 0;
static volatile uint32_t interrupt_loop_max_us = 0;

static inline void timing_update_max(volatile uint32_t &current_max, uint32_t value) {
  if (value > current_max) {
    current_max = value;
  }
}
#endif

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
  // In minimal variant TinyUSB HID instance 0 is the boot keyboard,
  // not the gamepad. Sending the 63-byte gamepad report to instance 0
  // here lands on the kbd interface and Windows interprets byte 0
  // (0x01) as Ctrl modifier plus stray scancodes -- visible as a
  // rogue keyboard hammering Ctrl/Win/etc after the first connect/
  // disconnect cycle. Only emit gamepad reports when full variant
  // is active (DS5 connected).
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
#ifdef ENABLE_WAKE_HID
  if (itf == usb_kbd_hid_instance()) {
    if (reqlen >= 8) {
      memset(buffer, 0, 8);
      return 8;
    }
    return 0;
  }
#endif
  (void)itf;
  (void)report_id;
  (void)report_type;
  (void)buffer;
  (void)reqlen;

  if (is_pico_cmd(report_id)) {
    return pico_cmd_get(report_id, buffer, reqlen);
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
#ifdef ENABLE_WAKE_HID
  if (itf == usb_kbd_hid_instance()) {
    // Drop keyboard SET_REPORT (host LED state).
    return;
  }
#endif
  (void)itf;
  (void)report_id;
  (void)report_type;
  (void)buffer;
  (void)bufsize;

  if (is_pico_cmd(report_id)) {
    printf("[HID] Receive 0xf6 setting config, funcid:0x%02X\n", buffer[0]);
    pico_cmd_set(report_id, buffer, bufsize);
    return;
  }

  // INTERRUPT OUT
  if (report_id == 0) {
    switch (buffer[0]) {
    case 0x02: {
      state_update(buffer + 1, bufsize - 1);
      if (spk_active) {
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
  // Set core voltage to 1.20V which is stable and safe for 320 MHz
  vreg_set_voltage(VREG_VOLTAGE_1_20);
  sleep_ms(1000);
  set_sys_clock_khz(SYS_CLOCK_KHZ, true);

  board_init();
  tusb_rhport_init_t dev_init = {.role = TUSB_ROLE_DEVICE,
                                 .speed = TUSB_SPEED_FULL};
  tusb_init(BOARD_TUD_RHPORT, &dev_init);
#if !ENABLE_SERIAL
  sleep_ms(150);
  tud_disconnect();
#endif
  board_init_after_tusb();
#if ENABLE_SERIAL
  stdio_usb_init();
#endif

  if (cyw43_arch_init()) {
    printf("Failed to initialize CYW43\n");
    return 1;
  }

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

#if !ENABLE_SERIAL
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
#endif

  // Initialize the critical section for the report buffer
  critical_section_init(&report_cs);
  wake_init();

  config_load();

  bt_init();
  bt_register_data_callback(on_bt_data);

  audio_init();
  state_init();

#if !ENABLE_SERIAL
  watchdog_enable(1000, true);
#endif

  while (1) {
#if ENABLE_DIAG
    static uint64_t last_loop_us = 0;
    const uint64_t loop_start_us = time_us_64();
    if (last_loop_us != 0) {
      timing_update_max(main_loop_gap_max_us,
                        static_cast<uint32_t>(loop_start_us - last_loop_us));
    }
    last_loop_us = loop_start_us;
#endif
#if !ENABLE_SERIAL
    watchdog_update();
#endif
#if ENABLE_DIAG
    uint64_t section_start_us = time_us_64();
#endif
    cyw43_arch_poll();
#if ENABLE_DIAG
    timing_update_max(cyw43_poll_max_us,
                      static_cast<uint32_t>(time_us_64() - section_start_us));
#endif
    bt_connection_watchdog_tick();
    bt_pump();
#if ENABLE_DIAG
    section_start_us = time_us_64();
#endif
    tud_task();
#if ENABLE_DIAG
    timing_update_max(tud_task_max_us,
                      static_cast<uint32_t>(time_us_64() - section_start_us));
#endif
    wake_task();
#ifdef ENABLE_WAKE_HID
    usb_variant_task();
#endif
#if ENABLE_DIAG
    section_start_us = time_us_64();
#endif
    audio_loop();
#if ENABLE_DIAG
    timing_update_max(audio_loop_max_us,
                      static_cast<uint32_t>(time_us_64() - section_start_us));
    section_start_us = time_us_64();
#endif
    interrupt_loop();
#if ENABLE_DIAG
    timing_update_max(interrupt_loop_max_us,
                      static_cast<uint32_t>(time_us_64() - section_start_us));
#endif
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

#if ENABLE_DIAG
void timing_get_diag(TimingDiag *out) {
  if (out == nullptr) return;
  out->main_loop_gap_max_us = main_loop_gap_max_us;
  out->cyw43_poll_max_us = cyw43_poll_max_us;
  out->tud_task_max_us = tud_task_max_us;
  out->audio_loop_max_us = audio_loop_max_us;
  out->interrupt_loop_max_us = interrupt_loop_max_us;
}

void timing_reset_diag() {
  main_loop_gap_max_us = 0;
  cyw43_poll_max_us = 0;
  tud_task_max_us = 0;
  audio_loop_max_us = 0;
  interrupt_loop_max_us = 0;
}
#endif
