//
// Created by awalol on 2026/3/5.
//

#ifndef DS5_BRIDGE_AUDIO_H
#define DS5_BRIDGE_AUDIO_H

#include <cstdint>

#if ENABLE_DIAG
struct AudioDiag {
    uint32_t audio_fifo_drops;
    uint32_t mic_fifo_drops;
    uint32_t mic_decode_fifo_drops;
    uint32_t mic_usb_partial_writes;
    uint32_t speaker_opus_reuses;
    uint32_t audio_report_gap_max_us;
    uint32_t opus_encode_max_us;
};
#endif

void audio_init();
void audio_loop();
void core1_entry();
void set_headset(bool state);
void mic_add_queue(uint8_t *data);
#if ENABLE_DIAG
void audio_get_diag(AudioDiag *out);
void audio_reset_diag();
#endif

#endif //DS5_BRIDGE_AUDIO_H
