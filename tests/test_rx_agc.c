/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "rx_limiter.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

enum { UNITY = 1 << 15, BLOCK = 160 };

static void level_block(int16_t samples[BLOCK], int16_t amplitude)
{
    for (unsigned i = 0; i < BLOCK; i++)
        samples[i] = i % 2 ? amplitude : -amplitude;
}

int main(void)
{
    voice_rx_limiter agc;
    int16_t input[BLOCK], output[BLOCK];
    voice_rx_limiter_reset(&agc);

    level_block(input, 600);
    for (unsigned i = 0; i < 50; i++)
        voice_rx_limiter_process(&agc, input, output, false);
    assert(!memcmp(input, output, sizeof input));
    assert(!agc.speech_rms_q8);

    for (unsigned i = 0; i < 36; i++)
        voice_rx_limiter_process(&agc, input, output, true);
    assert(output[BLOCK - 1] >= 2160 && output[BLOCK - 1] < 2200);
    for (unsigned i = 36; i < 50; i++)
        voice_rx_limiter_process(&agc, input, output, true);
    const int32_t speech_gain = agc.gain_q15;
    const int32_t speech_rms = agc.speech_rms_q8;
    assert(output[BLOCK - 1] >= 2190 && output[BLOCK - 1] < 2200);

    level_block(input, 40);
    voice_rx_limiter_process(&agc, input, output, false);
    assert(output[BLOCK - 1] == input[BLOCK - 1]);
    voice_rx_limiter_process(&agc, input, output, false);
    assert(!memcmp(input, output, sizeof input));

    level_block(input, 350);
    for (unsigned i = 0; i < 50; i++)
        voice_rx_limiter_process(&agc, input, output, false);
    assert(!memcmp(input, output, sizeof input));
    assert(agc.gain_q15 == speech_gain && agc.speech_rms_q8 == speech_rms);

    level_block(input, 600);
    voice_rx_limiter_process(&agc, input, output, true);
    assert(output[BLOCK - 1] >= 2190);
    input[0] = INT16_MIN;
    input[1] = INT16_MAX;
    voice_rx_limiter_process(&agc, input, output, false);
    assert(output[0] < -32000 && output[1] > 32000);

    voice_rx_limiter_reset(&agc);
    level_block(input, 2800);
    for (unsigned i = 0; i < 50; i++)
        voice_rx_limiter_process(&agc, input, output, true);
    assert(agc.gain_q15 > UNITY * 87 / 100 && agc.gain_q15 < UNITY * 89 / 100);
    for (unsigned i = 50; i < 500; i++)
        voice_rx_limiter_process(&agc, input, output, true);
    assert(output[BLOCK - 1] >= 2200 && output[BLOCK - 1] < 2210);

    voice_rx_limiter_reset(&agc);
    level_block(input, 300);
    for (unsigned i = 0; i < 50; i++)
        voice_rx_limiter_process(&agc, input, output, true);
    assert(agc.gain_q15 > UNITY * 398 / 100 && agc.gain_q15 <= 4 * UNITY);
    assert(output[BLOCK - 1] >= 1195 && output[BLOCK - 1] <= 1200);

    /* A quiet voiced lead-in followed by loud speech must shed the boost
     * within six 20 ms blocks. */
    level_block(input, 3000);
    for (unsigned i = 0; i < 6; i++) {
        const int32_t previous_gain = agc.gain_q15;
        voice_rx_limiter_process(&agc, input, output, true);
        assert(agc.gain_q15 < previous_gain);
    }
    assert(agc.gain_q15 <= UNITY);
    assert(output[BLOCK - 1] <= input[BLOCK - 1]);

    puts("PASS RX levelling response, quick cut after a quiet lead-in, pause protection and peak headroom");
}
