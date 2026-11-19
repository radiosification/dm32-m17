// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The UI's APRS services over core/aprs (ui_svc_aprs.h)
#include "config.h"
#if CONFIG_APRS
#include "ui_svc_aprs.h"
#include "aprs/aprs_fmt.h"
#include "ui_svc.h"
#include "board.h"
#include "storage/storage.h"
#include "hal_time.h"

bool ui_svc_aprs_view(aprs_view_t *v)
{
	aprs_get_view(v);
	return true;
}

void ui_svc_aprs_cfg(aprs_cfg_t *c)
{
	aprs_get_cfg(c);
}

// The path also lives in the codeplug's APRS record (aprs.md 11): written there by the housekeeping task
static void path_job(void *ctx)
{
	(void)ctx;
	(void)aprs_path_to_codeplug();
}

void ui_svc_aprs_set_cfg(const aprs_cfg_t *c)
{
	aprs_cfg_t old;
	aprs_get_cfg(&old);
	aprs_set_cfg(c, hal_millis());
	if (c->path != old.path) {
		(void)storage_job_post(path_job, NULL);	// busy with another job: the codeplug keeps its path
	}
}

// The Position screen reads the board GPS through APRS, the one place its NMEA goes
bool ui_svc_position(ui_position_t *out)
{
	aprs_gps_t g;
	if (!board_has_gps() || !aprs_gps(&g, hal_millis())) {
		return false;
	}
	out->fix = g.fix;
	out->sats = g.sats;
	out->lat_e7 = g.lat;
	out->lon_e7 = g.lon;
	out->alt_ok = g.alt_m != APRS_ALT_NONE;
	out->alt_m = out->alt_ok ? (int16_t)g.alt_m : 0;
	out->mode = g.mode;
	out->in_view = g.in_view;
	out->hdop10 = g.hdop10;
	out->speed_kmh = g.speed_kmh;
	out->course = g.course;
	out->utc_hms = g.utc_hms;
	return true;
}


bool ui_svc_aprs_send_now(void)
{
	return aprs_send_now();
}

#if CONFIG_APRS_RX
uint8_t ui_svc_aprs_station_count(void)
{
	return aprs_station_count();
}

bool ui_svc_aprs_station(uint8_t i, aprs_station_t *out)
{
	return aprs_station(i, out);
}
#endif
#endif
