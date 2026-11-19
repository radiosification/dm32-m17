// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Digital APRS (aprs_dmr.h)
#include <string.h>
#include "config.h"
#include "aprs/aprs_dmr.h"

bool aprs_dmr_send(bool udt, uint32_t dst, uint32_t own_id, const aprs_dmr_pos_t *pos)
{
	(void)udt;
	(void)dst;
	(void)own_id;
	(void)pos;
	return false;
}

