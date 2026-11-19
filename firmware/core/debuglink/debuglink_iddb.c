// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// DL_CMD_IDDB (docs/spec/debuglink.md 3.23): the DMR ID database as the radio sees it, its format and records, and the
// text the radio shows for an ID. A HIL test compares that with the database the host wrote, in either format
// (open-firmware-cps.md 7.5, iddb-compact.md), without a call on the air.
#include <string.h>
#include "config.h"
#include "debuglink/debuglink.h"
#include "debuglink/debuglink_proto.h"

size_t debuglink_cmd_iddb(const uint8_t *req, uint16_t len, uint8_t *out, size_t cap)
{
	(void)req;
	(void)len;
	(void)cap;
	out[0] = DL_ST_UNAVAILABLE;
	return 1;
}
