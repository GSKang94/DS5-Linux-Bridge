//
// bt_generic.h -- Generic BT HID host for 8BitDo standalone firmware.
//

#ifndef DS5_BRIDGE_BT_GENERIC_H
#define DS5_BRIDGE_BT_GENERIC_H

#include <cstdint>

enum CHANNEL_TYPE {
    INTERRUPT,
    CONTROL
};

typedef void (*bt_data_callback_t)(uint8_t slot, CHANNEL_TYPE channel,
                                   uint8_t *data, uint16_t len);

int bt_init(void);
void bt_register_data_callback(bt_data_callback_t callback);
void bt_write(uint8_t slot, const uint8_t *data, uint16_t len);
void bt_pump(void);
bool bt_send_pending(void);
bool bt_is_connected(void);
int bt_connected_count(void);
void bt_connection_watchdog_tick(void);
void bt_dualsense_power_off(void);

struct BtStatus {
    bool connected;
    bool is_dse;
    uint8_t battery_pct;
    bool charging;
    bool battery_valid;
    uint8_t addr[6];
};
void bt_get_status(uint8_t slot, BtStatus *out);

#endif // DS5_BRIDGE_BT_GENERIC_H
