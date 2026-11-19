// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The DMR parts of the debug link: the GET_VIEW sections dmr, dmr_call and dmr_stats, the voice path diagnostics
// lines and their counter reset. Kept apart from debuglink_view.c so DMR work can extend it on its own: add keys
// freely (new keys don't bump DL_VIEW_VERSION), keep the existing ones.
#include "config.h"
#include "board.h"
#include "debuglink/debuglink.h"


void debuglink_view_dmr(uint8_t part, const radio_view_t *v, dl_text_t *t)
{
	(void)part;
	(void)v;
	dl_text(t, "dmr.built=0\n");
}

