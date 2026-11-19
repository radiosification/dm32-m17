/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef M17_MIC_AUDIO_H
#define M17_MIC_AUDIO_H
#include <stdbool.h>
#include <stdint.h>

/* 8 kHz / 20 ms microphone processing, before Codec2. No heap or floating point.
 * NR is a speech-band filter and soft downward expander, not the proprietary
 * AMBE denoiser. It reduces noise in pauses, not noise buried under speech.
 * Options are latched/reset at PTT; the audio owner alone updates the state.
 */
typedef struct {
    uint8_t gain_step; /* 0..10, 5 = unity, 2 dB/step; ignored with AGC */
    bool agc, noise_reduction;
} m17_mic_audio_options;

typedef struct {
    m17_mic_audio_options options;
    int32_t hp_x, hp_y, lp_x1, lp_x2, lp_y1, lp_y2;
    uint32_t noise_rms, min_rms, gain_q12, gate_q12, output_gain_q12;
    uint32_t frames, limited_samples, input_peak, output_peak, input_rms;
} m17_mic_audio;

void m17_mic_audio_reset(m17_mic_audio *, m17_mic_audio_options);
void m17_mic_audio_process(m17_mic_audio *, int16_t pcm[160]);
#endif
