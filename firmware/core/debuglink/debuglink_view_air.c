// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The air section: the data service's TX queue (docs/spec/dmr-data.md 12), its post-TX guard, and zone sharing
// (docs/spec/zone-share.md). "airq.built=0" without the queue; "zs.built=0" without zone sharing
#include "config.h"
#include "debuglink/debuglink.h"

void debuglink_view_air(dl_text_t *t)
{
	dl_text(t, "airq.built=0\n");
	dl_text(t, "zs.built=0\n");
}
