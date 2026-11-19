// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The audio probe (CONFIG_AUDIO_PROBE; docs/spec/debuglink.md 3.14, docs/spec/hardware-dm1701.md 21.3): a bench tool to find
// out which audio-chip settings put FM receive audio on the PCM stream and PCM into the FM modulator. It reads and
// writes the audio chip's registers (board.audio_tap->modem_reg), runs the board's PCM stream with its own callback
// (optionally sending a test tone), and reports the levels of both I2S channels. It also reads and writes the chip's
// RAM (board.audio_tap->modem_ram: the HR_C6000's FM data buffers) and watches its interrupt flags and one RAM word
// for a while, to find the chip's own FM sample path (hardware-dm1701.md 21.3, candidate C). Diagnostics only: it borrows the
// stream, so it refuses while DMR voice owns it, and a beep in the meantime may stop it (status says so).
#include <string.h>
#include <math.h>
#include "config.h"
#include "board.h"
#include "debuglink/debuglink.h"
#include "debuglink/debuglink_proto.h"
#include "debuglink/dlog.h"
#include "hal_time.h"


size_t debuglink_cmd_audio_probe(const uint8_t *req, uint16_t len, uint8_t *out, size_t cap)
{
	(void)req;
	(void)len;
	(void)cap;
	out[0] = DL_ST_UNAVAILABLE;
	return 1;
}

