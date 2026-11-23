/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "rx_limiter.h"

enum {
    SAMPLES = 160,
    UNITY = 1 << 15,
    MIN_GAIN = UNITY / 2,
    MAX_GAIN = UNITY * 4,
    TARGET_RMS = 2200,
    MIN_SPEECH_RMS = 96,
    LOUD_SPEECH_RMS = 2800,
    SPEECH_HOLD_BLOCKS = 6,
    RMS_AVERAGING_BLOCKS = 36,
    LOUD_RMS_AVERAGING_BLOCKS = 4,
    ATTENUATION_BLOCKS = 60,
    LOUD_CUT_BLOCKS = 2,
    BOOST_BLOCKS = 10,
};

static unsigned sqrt_floor(unsigned n)
{
    unsigned root = 0;
    for (unsigned bit = 1u << 30; bit; bit >>= 2) {
        if (n >= root + bit) {
            n -= root + bit;
            root = (root >> 1) + bit;
        } else {
            root >>= 1;
        }
    }
    return root;
}

void voice_rx_limiter_reset(voice_rx_limiter *s)
{
    *s = (voice_rx_limiter){.gain_q15 = UNITY, .output_gain_q15 = UNITY};
}

void voice_rx_limiter_process(voice_rx_limiter *s, const int16_t in[SAMPLES],
                              int16_t out[SAMPLES], bool voiced)
{
    uint64_t energy = 0;
    int32_t peak = 0;
    for (unsigned i = 0; i < SAMPLES; i++) {
        int32_t sample = in[i];
        int32_t magnitude = sample < 0 ? -sample : sample;
        if (magnitude > peak)
            peak = magnitude;
        energy += (uint32_t)(sample * sample);
    }
    const int32_t rms = sqrt_floor(energy / SAMPLES);

    /* Learn on voiced Codec2 blocks only. The boost closes over 99% of
     * a steady gain step in 1 s of voiced audio. Attenuation has a
     * time constant of about 1.2 s. */
    if (voiced && rms >= MIN_SPEECH_RMS) {
        s->speech_hold = SPEECH_HOLD_BLOCKS;
        if (!s->speech_rms_q8)
            s->speech_rms_q8 = rms * 256;
        /* Drop inherited boost quickly when a clearly loud voiced block
         * arrives. Let the speech estimate catch up on the same blocks. */
        const bool cut_boost = rms >= LOUD_SPEECH_RMS && s->gain_q15 > UNITY;
        const int32_t average_blocks = cut_boost ? LOUD_RMS_AVERAGING_BLOCKS : RMS_AVERAGING_BLOCKS;
        s->speech_rms_q8 += (rms * 256 - s->speech_rms_q8) / average_blocks;
        int32_t desired = TARGET_RMS * UNITY / (s->speech_rms_q8 / 256);
        if (desired < MIN_GAIN)
            desired = MIN_GAIN;
        if (desired > MAX_GAIN)
            desired = MAX_GAIN;
        if (cut_boost && desired > UNITY)
            desired = UNITY;
        const int32_t delta = desired - s->gain_q15;
        const int32_t gain_blocks = cut_boost ? LOUD_CUT_BLOCKS
                                             : (delta < 0 ? ATTENUATION_BLOCKS : BOOST_BLOCKS);
        s->gain_q15 += delta / gain_blocks;
    } else if (s->speech_hold) {
        s->speech_hold--;
    }

    /* Preserve short unvoiced consonants. Taper positive gain away for
     * low-level blocks and pauses without changing the learned gain. */
    int32_t gain = s->gain_q15;
    if (gain > UNITY) {
        int32_t floor = s->speech_rms_q8 / (6 * 256);
        if (floor < MIN_SPEECH_RMS)
            floor = MIN_SPEECH_RMS;
        int32_t above_floor = rms - floor;
        if (!s->speech_hold || above_floor <= 0)
            gain = UNITY;
        else if (above_floor < 2 * floor)
            gain = UNITY + (gain - UNITY) * above_floor / (2 * floor);
    }

    /* Bound the interpolated gain by the block's headroom. Keep the
     * speaker PCM waveform and leave the decoder's peak control in place. */
    int32_t start = s->output_gain_q15;
    if (peak) {
        const int32_t safe_gain = INT16_MAX * UNITY / peak;
        if (start > safe_gain)
            start = safe_gain;
        if (gain > safe_gain)
            gain = safe_gain;
    }
    const int32_t step_q23 = (gain - start) * 256 / SAMPLES;
    int32_t ramp_q23 = start * 256;
    for (unsigned i = 0; i < SAMPLES; i++) {
        ramp_q23 += step_q23;
        out[i] = (int16_t)((int32_t)in[i] * (ramp_q23 / 256) / UNITY);
    }
    s->output_gain_q15 = gain;
}
