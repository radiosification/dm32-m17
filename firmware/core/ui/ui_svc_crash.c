// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The ui_svc.h calls for the crash log (core/crash/crash.h) and its report: the record packed (crash_pack) and sent as
// one UDP datagram through the DMR data service (dmr_data_send_udp, safe from any task) to the target the settings
// name. Built with the crash log only; otherwise the weak stubs stay.
#include "config.h"

#if CONFIG_CRASH_SCREEN
#include "ui_svc.h"
#include "crash/crash.h"

bool ui_svc_crash_get(ui_crash_t *out)
{
	const crash_record_t *r = crash_last();
	if (r == NULL)
		return false;
	out->seen = (r->flags & CRASH_F_SEEN) != 0;
	out->sent = (r->flags & CRASH_F_SENT) != 0;
	out->count = r->count;
	out->uptime_ms = r->uptime_ms;
	out->build = r->build;
	crash_summary(r, out->summary, sizeof out->summary);
	return true;
}

void ui_svc_crash_seen(void)
{
	crash_mark(CRASH_F_SEEN);
}

void ui_svc_crash_clear(void)
{
	crash_clear();
}

#endif
