/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef VOICE_RX_LIMITER_H
#define VOICE_RX_LIMITER_H
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    int32_t speech_rms_q8;
    int32_t gain_q15, output_gain_q15;
    unsigned speech_hold;
} voice_rx_limiter;

/* Reset once per received call. Process 20 ms of decoded 8 kHz PCM at a time. */
void voice_rx_limiter_reset(voice_rx_limiter *);
void voice_rx_limiter_process(voice_rx_limiter *, const int16_t in[160], int16_t out[160], bool voiced);
#endif
