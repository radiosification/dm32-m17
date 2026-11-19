/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef VOICE_RX_LIMITER_H
#define VOICE_RX_LIMITER_H
#include <stdint.h>

/* 8 kHz decoded speaker PCM, 160 samples per block. Slow AGC followed by
 * independent first-block burst protection and absolute output ceilings.
 * The whole decoded block is available before playback, so attenuation can
 * start at its FIRST sample without adding another frame of lookahead.
 * Peak and RMS ceilings are absolute AND relative to recent ordinary audio.
 * This bounds garble; it cannot recover speech from corrupt IMBE parameters.
 */
enum { VOICE_RX_PEAK_MAX=12000, VOICE_RX_RMS_MAX=3000,
       VOICE_RX_PEAK_MIN=2048, VOICE_RX_RMS_MIN=512,
       VOICE_RX_AGC_TARGET=1400, VOICE_RX_AGC_MAX_Q15=4*32768 };
typedef struct {
    uint32_t gain_q15, reference_peak, reference_rms;
    uint32_t agc_gain_q15;
    uint32_t frames, limited_frames, input_peak, output_peak;
} voice_rx_limiter;

/* Reset at a new receive call/session, not on each damaged frame.
 * Keeping history through erasures and pauses protects end-of-call tails.
 * Owner: the decoder worker or audio task, outside interrupts.
 */
void voice_rx_limiter_reset(voice_rx_limiter *);
void voice_rx_limiter_process(voice_rx_limiter *, const int16_t in[160], int16_t out[160]);
#endif
