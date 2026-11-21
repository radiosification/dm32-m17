/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "mic_audio.h"
#include <string.h>

static int32_t clamp(int32_t x, int32_t lo, int32_t hi)
{ return x < lo ? lo : x > hi ? hi : x; }
static uint32_t magnitude(int32_t x) { return (uint32_t)(x < 0 ? -x : x); }
static uint32_t isqrt(uint32_t n)
{
    uint32_t root = 0, bit = 1u << 30;
    while (bit > n) bit >>= 2;
    while (bit) {
        if (n >= root + bit) { n -= root + bit; root = (root >> 1) + bit; }
        else root >>= 1;
        bit >>= 2;
    }
    return root;
}
void m17_mic_audio_reset(m17_mic_audio *s, m17_mic_audio_options options)
{
    memset(s, 0, sizeof *s);
    if (options.gain_step > 10) options.gain_step = 10;
    s->options = options;
    s->gain_q12 = s->output_gain_q12 = 4096;
    s->min_rms = UINT32_MAX;
}
void m17_mic_audio_process(m17_mic_audio *s, int16_t pcm[160])
{
    /* Match the existing DMR manual gain steps. */
    static const uint16_t manual_q8[11] = {81,102,128,161,203,256,322,406,511,643,810};
    uint64_t energy = 0;
    const bool agc = s->options.agc;
    const bool filter_only = !agc && s->options.gain_step == 5;
    for (unsigned i = 0; i < 160; ++i) {
        int32_t x = pcm[i];
        if (magnitude(x) > s->input_peak) s->input_peak = magnitude(x);
        /* Always block DC and rumble before Codec2 (~80 Hz). */
        int32_t hp = clamp(x - s->hp_x + (int32_t)((int64_t)30772 * s->hp_y / 32768), -32768, 32767);
        s->hp_x = x;
        s->hp_y = hp;
        pcm[i] = (int16_t)hp;
        if (!filter_only) energy += (uint64_t)((int64_t)hp * hp);
    }
    if (filter_only) return;
    uint32_t rms = isqrt((uint32_t)(energy / 160));
    s->input_rms = rms;
    if (agc) {
        /* Track a bounded noise floor so AGC updates its gain on speech. */
        if (!s->frames) s->noise_rms = rms < 64 ? rms : 64;
        if (rms < s->min_rms) s->min_rms = rms;
        if (rms < s->noise_rms) s->noise_rms = (3 * s->noise_rms + rms) / 4;
        if ((s->frames + 1) % 25 == 0) {
            if (s->min_rms < s->noise_rms) s->noise_rms = s->min_rms;
            else {
                const uint32_t candidate = s->min_rms < 64 ? s->min_rms : 64;
                if (candidate > s->noise_rms) s->noise_rms += (candidate - s->noise_rms) / 8;
            }
            s->min_rms = UINT32_MAX;
        }
        if (rms > 3 * s->noise_rms + 16) {
            const uint32_t desired = (uint32_t)clamp((int32_t)(1800u * 4096u / rms), 1024, 6u * 4096u);
            /* Reduce gain quickly and bring it back more gradually. */
            const int32_t delta = (int32_t)desired - (int32_t)s->gain_q12;
            s->gain_q12 = (uint32_t)((int32_t)s->gain_q12 + delta / (delta < 0 ? 2 : 8));
        }
    }
    const uint32_t target = agc ? s->gain_q12 : (uint32_t)manual_q8[s->options.gain_step] * 16;
    const uint32_t start = s->output_gain_q12;
    const int32_t limit = agc ? 28000 : 32767;
    const int32_t low = agc ? -28000 : -32768;
    /* Interpolate each frame's gain to avoid steps at block boundaries. */
    for (unsigned i = 0; i < 160; ++i) {
        int32_t gain = (int32_t)start + ((int32_t)target - (int32_t)start) * (int32_t)(i + 1) / 160;
        int32_t y = (int32_t)((int64_t)pcm[i] * gain / 4096);
        if (y > limit || y < low) s->limited_samples++;
        pcm[i] = (int16_t)clamp(y, low, limit);
        if (magnitude(pcm[i]) > s->output_peak) s->output_peak = magnitude(pcm[i]);
    }
    s->output_gain_q12 = target;
    s->frames++;
}
