//
// Created by awalol on 2026/3/4.
//

#ifndef DS5_BRIDGE_BT_H
#define DS5_BRIDGE_BT_H

#include <cstdint>
#include <vector>

enum CHANNEL_TYPE {
    INTERRUPT,
    CONTROL
};

typedef void (*bt_data_callback_t)(CHANNEL_TYPE channel, uint8_t *data, uint16_t len);

int bt_init();
void bt_register_data_callback(bt_data_callback_t callback);
void bt_write(const uint8_t *data, uint16_t len, bool kick = true);
// Kick the BT send chain if pending. Called from main loop after
// cyw43_arch_poll() so the kick cost is paid outside audio_loop.
void bt_pump();
bool bt_send_pending();

// Live controller status for the web UI / Decky plugin (GET /api/status).
// All fields are cheap reads of data the firmware already tracks. battery_pct
// and charging are only meaningful when connected (and after the first 0x31
// report). (RSSI was intentionally omitted: BR/EDR HCI_Read_RSSI is relative to
// the Golden Receive Power Range and reads ~0 in normal use -- not a useful
// signal-strength number to surface.)
struct BtStatus {
    bool    connected;
    bool    is_dse;       // true = DualSense Edge, false = standard DualSense
    uint8_t battery_pct;  // 0-100 (DS5 reports in 10% steps); 0 if unknown
    bool    charging;     // true while the controller is charging or full
    bool    battery_valid;// false until a fresh input report has been seen
};
void bt_get_status(BtStatus *out);
std::vector<uint8_t> get_feature_data(uint8_t reportId,uint16_t len);
void init_feature();
void set_feature_data(uint8_t reportId, uint8_t* data,uint16_t len);

// Accessors used by the DSE profile module (dse.cpp). Defined in bt.cpp.
uint16_t bt_control_cid();          // current HID control channel id (0 if none)
void bt_control_send(const uint8_t *data, uint16_t len);

// Tells the connected DualSense to power off (same as a long-press of the
// PS button). No-op if no controller is connected. Used on host-suspend so
// the controller doesn't sit awake until its idle timer fires.
void bt_dualsense_power_off();

// Tick connection watchdog. Call from main loop.
void bt_connection_watchdog_tick();

//--------------------------------------------------------------------+
// Paired-device (bond) management, exposed to the web config UI.
// Bonds are BR/EDR link keys persisted by BTstack in its flash TLV bank
// (capacity NVM_NUM_LINK_KEYS). These wrap the BTstack gap_* link-key API so
// web_api.cpp doesn't pull in btstack headers. All run on the core0 main-loop
// context (same as the btstack run loop), so no extra locking is needed.
//--------------------------------------------------------------------+

// Number of bytes in a Bluetooth address (matches btstack bd_addr_t).
#define BT_ADDR_LEN 6

// Copy up to `max` stored bond addresses into addrs (each BT_ADDR_LEN bytes).
// Returns the number written.
int bt_bond_list(uint8_t (*addrs)[BT_ADDR_LEN], int max);

// Forget a single bond by address (6 bytes). Returns true if it was issued.
bool bt_bond_forget(const uint8_t *addr);

// Forget every stored bond.
void bt_bond_forget_all();

// Force a fresh 30s inquiry to pair an additional controller, even when one is
// already bonded. No-op while a controller is connected. Invoked from the web
// API (POST /api/bonds action=pair); normally the dongle only inquires when no
// controller is bonded.
void bt_start_pairing();

// If a controller is currently connected, copy its address into addr_out
// (BT_ADDR_LEN bytes) and return true; otherwise return false.
bool bt_connected_addr(uint8_t *addr_out);

// Flush the forgotten-controller blacklist to flash if it changed (deferred
// from the HID-open hot path). Call every main-loop iteration.
void bt_blacklist_persist_if_dirty();

#endif //DS5_BRIDGE_BT_H
