// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Microphone gain (mic_gain.h; docs/spec/radio-features.md 3.6)
#include "radio/mic_gain.h"

static volatile uint8_t mic_step = MIC_GAIN_UNITY;

void radio_set_mic_gain(uint8_t step)
{
	if (step > MIC_GAIN_STEPS) {
		step = MIC_GAIN_STEPS;
	}
	mic_step = step;
	bool hw = mic_gain_on_board();
	if (hw) {
		board.radio->mic_gain(mic_gain_offset_db(step));
	}
}

uint8_t radio_mic_gain(void)
{
	return mic_step;
}
