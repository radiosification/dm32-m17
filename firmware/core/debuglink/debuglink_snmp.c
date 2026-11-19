// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The SNMP agent on the debug link (docs/spec/debuglink.md 3.17, docs/spec/snmp.md 7): the snmp view section (the
// flags and the transport counters; the protocol counters and the other settings are in the MIB itself) and the
// SNMP command (settings, communities, the host's notification port, a test notification). Both run in the host link
// task, as the agent does.
#include <string.h>
#include "config.h"
#include "debuglink/debuglink.h"
#include "debuglink/debuglink_proto.h"

void debuglink_view_snmp(dl_text_t *t)
{
	dl_text(t, "snmp.built=0\n");
}

size_t debuglink_cmd_snmp(const uint8_t *req, uint16_t len, uint8_t *out, size_t cap)
{
	(void)req;
	(void)len;
	(void)cap;
	out[0] = DL_ST_UNAVAILABLE;
	return 1;
}

