// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The roam view section (docs/spec/debuglink.md 3.7, docs/spec/dmr-roaming.md 6.3): DMR repeater roaming's state,
// the sites with their last measurements, the counters and the roaming settings
#include "config.h"
#include "debuglink/debuglink.h"


void debuglink_view_roam(const radio_view_t *v, dl_text_t *t)
{
	(void)v;
	dl_text(t, "roam.built=0\n");
}
