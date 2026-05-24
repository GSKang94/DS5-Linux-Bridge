//
// Created by awalol on 2026/5/15.
//

#ifndef DS5_BRIDGE_STATE_MGR_H
#define DS5_BRIDGE_STATE_MGR_H

void state_init();
// Copy the cached 63-byte state into `data` (which must be at least `size`
// bytes). Despite the historical name, this is a *getter* — host inputs go
// through state_update(), not here.
void state_get(uint8_t *data, uint8_t size);
void state_update(const uint8_t *data, const uint8_t size);

// Shared global state variables for hybrid muting
extern volatile bool g_firmware_mic_muted;
extern volatile bool g_host_hid_manages_mute;
extern volatile uint8_t g_last_uac_mute;

// Mute control helper functions
void state_set_local_mute(bool muted);
void state_reset_mute();
void state_push_to_bt();

#endif //DS5_BRIDGE_STATE_MGR_H
