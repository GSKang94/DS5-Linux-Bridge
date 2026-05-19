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

#endif //DS5_BRIDGE_STATE_MGR_H
