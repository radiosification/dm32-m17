// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The DMR data parts of the debug link (docs/spec/debuglink.md 3.7 dmr_data, 3.12 DMR_DATA): the service's state and
// counters as key=value text, and requests (send an SMS, call alert, radio check, settings, read the inbox) for the
// HIL tests. Requests go through dmr_data_api.h, so they are handled by the radio task at its next tick.
#include <string.h>
#include "config.h"
#include "debuglink/debuglink.h"
#include "debuglink/debuglink_proto.h"


void debuglink_view_dmr_data(uint8_t part, dl_text_t *t)
{
	dl_text(t, part == 2 ? "ip.built=0\n" : "data.built=0\n");
}

size_t debuglink_cmd_dmr_data(const uint8_t *req, uint16_t len, uint8_t *out, size_t cap)
{
	(void)req;
	(void)len;
	(void)cap;
	out[0] = DL_ST_UNAVAILABLE;
	return 1;
}

void debuglink_dmr_data_reset(void)
{
}

