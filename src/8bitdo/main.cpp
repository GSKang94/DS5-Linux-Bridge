//
// main.cpp -- 8BitDo Pro 2 bridge firmware entry point.
//
// Simple main loop: BT Classic HID host -> USB HID device passthrough,
// plus WiFi config web server and TV control.
//

#include "bsp/board_api.h"
#include "bt.h"
#include "config.h"
#include "flash_safety.h"
#include "hardware/clocks.h"
#include "hardware/watchdog.h"
#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"
#include "tusb.h"
#include "tv_control.h"
#include "web_api.h"
#include "wifi_net.h"
#include <cstdio>
#include <cstring>

// Latest input report from the controller (report ID 3, up to 64 bytes).
static uint8_t last_report[64];
static uint8_t last_report_len = 0;
static volatile bool report_pending = false;

// bt.cpp's bt_get_status() reads interrupt_in_data[slot][52] for DS5 battery.
// In the 8BitDo build this is unused but must exist to satisfy the linker.
uint8_t interrupt_in_data[1][63] = {};

//--------------------------------------------------------------------+
// bridge_reset_slot_input -- required by slots.h / bt.cpp
//--------------------------------------------------------------------+
void bridge_reset_slot_input(uint8_t slot) {
    (void)slot;
    memset(last_report, 0, sizeof(last_report));
    last_report_len = 0;
    report_pending = false;
}

//--------------------------------------------------------------------+
// BT data callback: receives raw HID interrupt data from the controller
//--------------------------------------------------------------------+
static void on_bt_data(uint8_t slot, CHANNEL_TYPE channel, uint8_t *data, uint16_t len) {
    (void)slot;
    if (channel != INTERRUPT) return;
    // Raw L2CAP HID data: first byte is the HID transaction header (0xA1 for
    // DATA | INPUT). The actual HID report follows at data[1..].
    if (len < 2) return;
    if (data[0] != 0xA1) return;

    // Copy the report (skip the 0xA1 header byte)
    uint16_t rlen = len - 1;
    if (rlen > sizeof(last_report)) rlen = sizeof(last_report);
    memcpy(last_report, data + 1, rlen);
    last_report_len = (uint8_t)rlen;
    report_pending = true;
}

//--------------------------------------------------------------------+
// TinyUSB HID callbacks
//--------------------------------------------------------------------+

// GET_REPORT callback (host pulls)
uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
                                hid_report_type_t report_type,
                                uint8_t *buffer, uint16_t reqlen) {
    (void)instance;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)reqlen;
    return 0;
}

// SET_REPORT callback (host pushes output/feature reports)
void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                            hid_report_type_t report_type,
                            uint8_t const *buffer, uint16_t bufsize) {
    (void)instance;
    // Report ID 5 = rumble output. Forward to controller over BT.
    if (report_type == HID_REPORT_TYPE_OUTPUT && report_id == 0x05) {
        // Build a SET_REPORT HID control packet: 0x53 (SET_REPORT|OUTPUT) + data
        uint8_t pkt[68];
        uint16_t pkt_len = bufsize + 2;
        if (pkt_len > sizeof(pkt)) pkt_len = sizeof(pkt);
        pkt[0] = 0x53; // SET_REPORT | OUTPUT
        pkt[1] = report_id;
        memcpy(pkt + 2, buffer, pkt_len - 2);
        bt_write(0, pkt, pkt_len, true);
    }
}

//--------------------------------------------------------------------+
// Main
//--------------------------------------------------------------------+

int main() {
    board_init();

    // System clock: 200 MHz (mild overclock for RP2350, stays at default 1.10V)
    set_sys_clock_khz(200000, true);

    stdio_init_all();
    printf("\n[8BitDo Bridge] Booting...\n");

    // CYW43 init (BT + WiFi)
    if (cyw43_arch_init()) {
        printf("CYW43 init failed!\n");
        return -1;
    }

    // Load config from flash
    config_load();

    // Init WiFi + web server
    wifi_net_init();
    tv_control_init();

    // Init BT
    bt_init();
    bt_register_data_callback(on_bt_data);

    // Init USB device
    tusb_rhport_init(0, NULL);
    tud_connect();

    printf("[8BitDo Bridge] Ready. Waiting for controller...\n");

    // POST LED pattern: 3 rapid flashes
    for (int i = 0; i < 6; i++) {
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, i % 2 == 0);
        sleep_ms(80);
    }
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, false);

    watchdog_enable(1000, true);

    while (1) {
        watchdog_update();
        cyw43_arch_poll();
        bt_connection_watchdog_tick();
        bt_blacklist_persist_if_dirty();
        bt_pump();
        tud_task();

        // Forward pending BT report to USB
        if (report_pending && tud_hid_n_ready(0)) {
            // Send with report ID (first byte of last_report is the report ID)
            tud_hid_n_report(0, last_report[0], last_report + 1,
                             last_report_len - 1);
            report_pending = false;
        }

        // WiFi + web tasks
        wifi_net_task();
        tv_control_task();

        // Yield when idle
        if (!bt_send_pending()) {
            sleep_us(250);
        }
    }
}
