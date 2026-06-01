//
// Created by awalol on 2026/5/4.
//

#ifndef DS5_BRIDGE_CMD_H
#define DS5_BRIDGE_CMD_H

#include <stdint.h>

#if ENABLE_DIAG
struct TimingDiag {
    uint32_t main_loop_gap_max_us;
    uint32_t cyw43_poll_max_us;
    uint32_t tud_task_max_us;
    uint32_t audio_loop_max_us;
    uint32_t interrupt_loop_max_us;
};
#endif

bool is_pico_cmd(uint8_t report_id);
uint16_t pico_cmd_get(uint8_t report_id, uint8_t *buffer,uint16_t reqlen);
void pico_cmd_set(uint8_t report_id, uint8_t const *buffer,uint16_t bufsize);
#if ENABLE_DIAG
void timing_get_diag(TimingDiag *out);
void timing_reset_diag();
#endif

#endif //DS5_BRIDGE_CMD_H
