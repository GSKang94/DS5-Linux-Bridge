//
// main_8bitdo.cpp -- 8BitDo standalone firmware entry point.
//
// Stripped from DS5's main.cpp: no audio, no haptics, no DSE, no multi-slot.
// Just BT HID passthrough + WiFi/WoL/TV control.
//

#include "bt_generic.h"
#include "bsp/board_api.h"
#include "hardware/watchdog.h"
#include "pico/cyw43_arch.h"
#include "tusb.h"

#include "config.h"
#include "tv_control.h"
#include "wake.h"
#include "wifi_net.h"

#include <cstdio>
#include <cstring>

// ─── HID report buffer ───────────────────────────────────────────────────────

// 8BitDo Pro 2 D-input max report: 31 bytes (report ID 4 / gyro)
#define MAX_REPORT_LEN 64
static uint8_t last_report[MAX_REPORT_LEN];
static uint16_t last_report_len = 0;
static volatile bool report_ready = false;

// ─── BT data callback ────────────────────────────────────────────────────────

static void on_bt_data(uint8_t slot, CHANNEL_TYPE channel, uint8_t *data, uint16_t len) {
    (void)slot;
    if (channel != INTERRUPT || len < 3) return;

    // BT HID: data[0]=0xA1 (DATA|INPUT), data[1]=report_id, data[2..]=payload
    // Forward everything after the 0xA1 header
    uint16_t usb_len = len - 1;
    if (usb_len > MAX_REPORT_LEN) usb_len = MAX_REPORT_LEN;

    memcpy(last_report, data + 1, usb_len);
    last_report_len = usb_len;
    report_ready = true;

    // Wake detection: any input while host is suspended can trigger wake
    wake_on_bt_input(data + 1, len - 1);
}

// ─── USB HID callbacks ───────────────────────────────────────────────────────

// Report descriptor (8BitDo Pro 2 D-input, 150 bytes)
extern const uint8_t desc_hid_report_8bitdo[];
extern const size_t desc_hid_report_8bitdo_len;

uint8_t const *tud_hid_descriptor_report_cb(uint8_t itf) {
    (void)itf;
    return desc_hid_report_8bitdo;
}

uint16_t tud_hid_get_report_cb(uint8_t itf, uint8_t report_id,
                               hid_report_type_t report_type,
                               uint8_t *buffer, uint16_t reqlen) {
    (void)itf; (void)report_id; (void)report_type;
    (void)buffer; (void)reqlen;
    return 0;
}

void tud_hid_set_report_cb(uint8_t itf, uint8_t report_id,
                           hid_report_type_t report_type,
                           uint8_t const *buffer, uint16_t bufsize) {
    (void)itf; (void)report_type;
    // Forward output reports (rumble) back to controller over BT
    if (report_id == 0x05 && bufsize >= 4) {
        // Wrap in BT HID output header: 0xA2 + report_id + payload
        uint8_t bt_out[6] = {0xA2, 0x05};
        memcpy(bt_out + 2, buffer, 4);
        bt_write(0, bt_out, 6);
    }
}

// ─── USB suspend/resume for TV control ───────────────────────────────────────

extern "C" void tud_suspend_cb(bool remote_wakeup_en) {
    (void)remote_wakeup_en;
    printf("[USB] Suspend\n");
}

extern "C" void tud_resume_cb(void) {
    printf("[USB] Resume\n");
    tv_on_host_wake();
}

extern "C" void tud_mount_cb(void) {
    printf("[USB] Mounted\n");
}

// ─── Main ────────────────────────────────────────────────────────────────────

int main() {
    board_init();

    if (cyw43_arch_init()) {
        printf("CYW43 init failed\n");
        return -1;
    }

    printf("\n=== 8BitDo Bridge ===\n");

    tusb_init();
    config_load();
    wifi_net_init();
    tv_control_init();

    const bool ap_onboarding = wifi_net_in_ap_mode();
    if (!ap_onboarding) {
        bt_init();
        bt_register_data_callback(on_bt_data);
    }

    wake_init();
    tud_connect();

    watchdog_enable(1000, true);

    // AP onboarding loop (no BT)
    if (ap_onboarding) {
        while (1) {
            watchdog_update();
            cyw43_arch_poll();
            tud_task();
            wifi_net_task();
            sleep_us(500);
        }
    }

    // Main loop
    while (1) {
        watchdog_update();
        cyw43_arch_poll();
        bt_connection_watchdog_tick();
        bt_pump();
        tud_task();

        // Forward HID report to USB
        if (report_ready && tud_hid_n_ready(0)) {
            uint8_t rid = last_report[0];
            tud_hid_n_report(0, rid, last_report + 1, last_report_len - 1);
            report_ready = false;
        }

        wifi_net_task();
        tv_control_task();
        wake_task();

        sleep_us(250);
    }
}
