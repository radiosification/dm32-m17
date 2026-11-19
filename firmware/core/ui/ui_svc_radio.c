// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The ui_svc.h calls the radio core answers: the talk target and the one-shot target of the focused row
// (docs/spec/dmr-voice.md), the temporary channel changes (timeslot, color code, mode), the hear all, monitor and VOX
// latches, privacy with the stored key, scan, key beeps and the radio's settings (docs/spec/radio-features.md).
// These are the strong definitions; ui_svc_stub.c keeps the weak ones for what other packages still have to provide.
#include "ui_svc.h"
#include "board.h"
#include "radio/radio.h"
#include "radio/beep.h"
#include "radio/vibe.h"
#include "radio/mic_gain.h"
#include "radio/ref_trim.h"
#include "radio/radio_cwid.h"
#include "radio/radio_bcast.h"
#include "input/input.h"
#include "settings/settings.h"
#include <string.h>
#include "config.h"
#if CONFIG_APRS
#include "aprs/aprs.h"
#endif

#if CONFIG_FM
bool ui_svc_mode(uint8_t mode)
{
	if (mode != RADIO_MODE_FM && mode != RADIO_MODE_DMR && !radio_vfo_mode_built(mode)) {
		return false;
	}
	radio_req_mode(mode);
	return true;
}
#endif
uint32_t ui_svc_feat_voice(void)
{
	uint32_t f = CONFIG_FM ? UI_FEAT_MONITOR_LATCH : 0;
	if(CONFIG_M17&&CONFIG_FM)f|=UI_FEAT_MODE;
	if (CONFIG_VOX) {
		f |= UI_FEAT_VOX;
	}
	if (CONFIG_DMR) {
		f |= UI_FEAT_TALK_TARGET | UI_FEAT_TIMESLOT | UI_FEAT_COLOR_CODE | UI_FEAT_HEAR_ALL;
		if (CONFIG_FM) {
			f |= UI_FEAT_MODE;
		}
		if (CONFIG_DMR_PRIVACY) {
			f |= UI_FEAT_PRIVACY;
		}
	}
	return f;
}

// The radio core's settings, read again for every change so one made elsewhere (the debug link, a host, the radio
// core) is kept. radio_get_settings answers with the last ones set until the radio task has taken them in, so a run
// of changes (ui_svc_apply, ui_svc_vox) never reads back a value from before its own last one. Static: off the UI
// task's stack
static radio_settings_t rs;

static radio_settings_t *rs_copy(void)
{
	radio_get_settings(&rs);
	return &rs;
}

static void rs_send(const radio_settings_t *was)
{
	if (memcmp(was, &rs, sizeof rs) != 0) {
		radio_set_settings(&rs);
	}
}

#if CONFIG_FM
// FM monitor latch: the squelch held open until off (a press of the MONITOR key ends it too)
bool ui_svc_monitor(bool on)
{
	radio_req_monitor(on);
	return true;
}
#endif




uint32_t ui_svc_feat_radio(void)
{
	bool motor = CONFIG_VIBRATE && board.power != NULL && board.power->vibrate != NULL;
	return (CONFIG_SCAN ? UI_FEAT_SCAN : 0) | (CONFIG_DMR_ROAMING ? UI_FEAT_ROAMING : 0) |
	       (motor ? UI_FEAT_VIBRATE : 0) | (radio_volume_soft() ? UI_FEAT_VOLUME : 0) |
	       (radio_bcast_capable() ? UI_FEAT_BCAST : 0) | (CONFIG_APRS && board_has_gps() ? UI_FEAT_GPS : 0) |
	       (ref_trim_available() ? UI_FEAT_REF_TRIM : 0);
}


#if CONFIG_SCAN
// The UI only asks for "on" when its copy of view.scan says the radio isn't scanning, so TOGGLE starts the zone
// scan (or the VFO band scan in the VFO) the way the radio core chooses
bool ui_svc_scan(bool on)
{
	radio_req_scan(on ? RSCAN_TOGGLE : RSCAN_STOP);
	return true;
}

bool ui_svc_scan_skip(void)
{
	radio_req_scan(RSCAN_NUISANCE);
	return true;
}
#endif

#if CONFIG_DUAL_WATCH
// The radio core alternates the one receiver between the two channels (radio-features.md 2.1). The same pair again
// only changes the selection there, so this is cheap to call on every change. Refused here, where the answer is
// needed now (the request itself is queued): the same channel twice, a channel that can't be read
bool ui_svc_dual(const ui_dual_ref_t ref[2], uint8_t sel)
{
	if (ref == NULL) {
		radio_req_dual(NULL, 0);
		return true;
	}
	static zone_t z;	// static: not on the UI task's stack
	static channel_t ch;
	radio_dual_ref_t r[2];
	for (uint8_t i = 0; i < 2; i++) {
		r[i] = (radio_dual_ref_t){ ref[i].vfo, ref[i].vfo ? 0 : ref[i].zone,
					   ref[i].vfo ? (uint16_t)(ref[i].pos & 1u) : ref[i].pos };	// a VFO: which
		if (r[i].vfo) {
			if (!(CONFIG_VFO_FM || CONFIG_VFO_DMR))
				return false;
		} else if (r[i].pos >= CODEPLUG_ZONE_MAX || !codeplug_zone(r[i].zone, &z) || r[i].pos >= z.count ||
			   !codeplug_channel(z.channels[r[i].pos], &ch)) {
			return false;
		}
	}
	if (sel > 1 || (r[0].vfo == r[1].vfo && r[0].zone == r[1].zone && r[0].pos == r[1].pos))
		return false;
	radio_req_dual(r, sel);
	return true;
}

bool ui_svc_dual_get(ui_dual_ref_t ref[2], uint8_t *sel)
{
	radio_dual_ref_t r[2];
	uint8_t at;
	if (!radio_get_dual(r, sel, &at))
		return false;
	for (uint8_t i = 0; i < 2; i++)
		ref[i] = (ui_dual_ref_t){ r[i].vfo, r[i].zone, r[i].pos };
	return true;
}

// Two with a second receiver (board.rx2: the radio core gives it the channel not selected), else one: dual watch
// alternates
uint8_t ui_svc_receivers(void)
{
	return board.rx2 != NULL ? 2 : 1;
}

// Each pane's own receiver: the main one on the selected pane, the second on the other. False while the second
// receiver doesn't have its channel (down, or not yet): the screens then show the main receiver alone
bool ui_svc_dual_rx(uint8_t pane, ui_dual_rx_t *out)
{
	static radio_view_t v;	// the UI task's only; too big for its stack
	radio_dual_ref_t ref[2];
	uint8_t sel, at;
	memset(out, 0, sizeof(*out));
	if (board.rx2 == NULL || pane > 1 || !radio_get_dual(ref, &sel, &at)) {
		return false;
	}
	radio_get_view(&v);
	if (!v.rx2) {
		return false;
	}
	if (pane == sel) {
		out->active = out->heard = v.state == RS_RX;
		out->rssi_dbm = v.rssi_dbm;
		out->dmr = v.ch.mode == RADIO_MODE_DMR;
		return true;
	}
	rx2_status_t st;
	board.rx2->status(&st);
	out->active = st.tuned && st.active;
	out->heard = out->active && st.heard;
	out->dmr = st.dmr;
	out->group = st.group;
	out->rssi_dbm = st.rssi_dbm;
	out->src = st.src;
	out->dst = st.dst;
	return true;
}

bool ui_svc_vfo_get(uint8_t index, channel_t *out)
{
	return radio_get_vfo(index, out);
}
#endif

#if CONFIG_BEEP
void ui_svc_beep(uint8_t beep)
{
	static const uint8_t map[] = {
		[UI_BEEP_KEY] = BEEP_KEY, [UI_BEEP_ERROR] = BEEP_ERROR, [UI_BEEP_MESSAGE] = BEEP_MESSAGE,
		[UI_BEEP_BATTERY_LOW] = BEEP_BATTERY_LOW,
	};
	radio_req_beep(beep < sizeof map ? map[beep] : BEEP_KEY);
}
#endif


void ui_svc_ptt_block(bool blocked)
{
	input_set_ptt_block(blocked);
}

void ui_svc_dtmf_block(bool blocked)
{
	input_set_dtmf_block(blocked);
}

// The reply window the Settings screen shows: the radio's setting, else the codeplug's group hang time, else the
// call stack's 10 s
int32_t ui_svc_reply_shown(const radio_settings_t *s)
{
	if (s->reply_s <= 30u) {
		return s->reply_s;
	}
	return 10;
}

// Mic gain: the step, or step 5 (the board's levels, FM included) while the vocoder's AGC sets the level (Mic gain
// dimmed)
static void apply_mic_gain(int32_t step, bool agc)
{
	if (CONFIG_DMR && agc) {
		step = (int32_t)MIC_GAIN_UNITY;
	} else if (step < 0) {
		step = 0;
	}
	radio_set_mic_gain((uint8_t)(step > (int32_t)MIC_GAIN_STEPS ? (int32_t)MIC_GAIN_STEPS : step));
}

// The UI's settings that live in radio_settings_t. The radio core saves them (settings store) and restores them at
// start-up, so ui_svc_setting_get reads them back from it (ui_svc_storage.c) and a value it already has is not sent
// again. Only the field the setting names changes (rs_copy)
void ui_svc_apply(uint8_t id, int32_t value)
{
	radio_settings_t was = *rs_copy();
	uint16_t v16 = (uint16_t)(value < 0 ? 0 : value > 0xFFFF ? 0xFFFF : value);
	switch (id) {
	case UI_SET_KEY_TONES:
		rs.key_beep = value != 0;
		break;
	case UI_SET_TALK_PERMIT:
		rs.talk_permit = value != 0;
		break;
	case UI_SET_VFO_STEP:		// 10 Hz units; 833 is the exact 8.33 kHz grid
		radio_req_vfo_step_size(value == 833 ? 8333u : value > 0 && value <= 100000 ? (uint32_t)value * 10u : 0u);
		return;
	case UI_SET_MIC_GAIN:		// the board's gain (FM and DMR) or DMR's sample scaling (radio/mic_gain.h)
		apply_mic_gain(value, ui_svc_setting_get(UI_SET_MIC_AGC) != 0);
		return;
	case UI_SET_REF_TRIM:		// the board's reference, receive and transmit (radio/ref_trim.h)
		if (radio_set_ref_trim(value))
			radio_req_retune();	// a board that applies it when it tunes
		return;
	case UI_SET_TONE_VOLUME:		// 0..10
		rs.beep_level = (uint8_t)(v16 > 10 ? 100 : v16 * 10u);
		break;
	case UI_SET_TOT_S:
		rs.tot_s = v16;
		break;
#if CONFIG_DUAL_WATCH
	case UI_SET_DUAL_DELAY_MS:	// the radio core clamps
		rs.dual_delay_ms = v16;
		break;
#endif
	case UI_SET_BACKLIGHT_S:		// dims after this, goes off after four times as long (0 = always on)
		rs.backlight_dim_s = v16;
		rs.backlight_off_s = (uint16_t)(v16 > 0x3FFF ? 0xFFFF : v16 * 4u);
		break;
	case UI_SET_BATTERY_SAVER:
		rs.rx_save = value != 0 ? (rs.rx_save != 0 ? rs.rx_save : 1) : 0;
		break;
	case UI_SET_REPLY_S:		// 0..30 s. The value shown for "the codeplug's" (at start-up) keeps it so
		if (value != ui_svc_reply_shown(&rs)) {
			rs.reply_s = value >= 0 && value <= 30 ? (uint8_t)value : RADIO_REPLY_CODEPLUG;
		}
		break;
	case UI_SET_GPS:		// the module's power and the position sources, now (not a radio core setting)
		board_gps_enable(value != 0);
#if CONFIG_APRS
		aprs_gps_enable(value != 0);
#endif
		return;
	case UI_SET_LONG_PRESS_MS:	// the input layer's gesture timing (not a radio core setting)
	case UI_SET_REPEAT_MS: {
		static uint16_t long_ms, repeat_ms;
		if (id == UI_SET_LONG_PRESS_MS) {
			long_ms = v16;
		} else {
			repeat_ms = v16;
		}
		input_set_timing(long_ms, repeat_ms);
		return;
	}
	default:
		return;
	}
	rs_send(&was);
}
