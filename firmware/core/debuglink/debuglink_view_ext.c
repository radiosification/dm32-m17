// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The dmr_ext view section (docs/spec/debuglink.md 3.7): DMR emergency (core/dmr/dmr_emergency.h), the ETSI short
// data counters and settings (core/dmr/dmrDataService.h) and the hotspot's CW ID (core/radio/radio_hotspot.h). Apart
// from dmr_data so that section keeps fitting one reply.
#include "config.h"
#include "debuglink/debuglink.h"


void debuglink_view_dmr_ext(dl_text_t *t)
{
	dl_text(t, "em.built=0\n");
}
