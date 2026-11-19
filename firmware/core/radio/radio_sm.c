// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The radio core's state machine: channel / zone / VFO selection, software squelch, PTT checks and the TX timer,
// and the everyday features around them (docs/spec/radio-features.md): scan, busy channel lockout, roger beep and
// 1750 Hz burst, VOX, beeps, the battery monitor and receiver battery saving.
// No RTOS here: radio_task.c serializes the calls, and the host tests drive it directly.
#include <string.h>
#include "board.h"
#include "radio/radio_sm.h"
#include "radio/tones.h"
#include "settings/settings.h"
#include "config.h"
#if CONFIG_M17
#include "dv/radio_m17.h"
#endif
#include "hal_dtmf.h"
#if CONFIG_ZONE_WRITE
#include "codeplug/zone_edit.h"
#define ZONE_WRITE_BUSY()	zone_edit_busy()
#define ZONES_MOVED(sm)		(zone_edit_shifts() != (sm)->zone_shifts)
#else
#define ZONE_WRITE_BUSY()	false
#define ZONES_MOVED(sm)		false
#endif
#include "radio/beep.h"	// beep_t (beep_play only with CONFIG_BEEP)
#include "radio/vibe.h"	// vibe_t (vibe_play only with CONFIG_VIBRATE)
#if RADIO_SM_DV
#include "dv/radio_dv.h"	// P25 and NXDN channels (the multi-mode derivative)
#endif
#if CONFIG_APRS_RX
#include "usbaudio/audio_mux.h"	// the PCM stream shared with a host audio session and APRS
#endif
#if CONFIG_APRS
#include "aprs/afsk.h"			// AFSK_BAUD, the tones
#endif

// The radio's own DMR ID when the codeplug's general settings hold none (build value; 0 = refuse DMR transmit)
#ifndef RADIO_DMR_ID_DEFAULT
#define RADIO_DMR_ID_DEFAULT	0
#endif

// Simplex frequencies tried, in order, for the VFO when there is nothing better to start from
// The VFO is analog (CONFIG_VFO_FM), digital (CONFIG_VFO_DMR) or both, with the mode a setting of the VFO itself
#define VFO_AVAILABLE	(CONFIG_VFO_FM || CONFIG_VFO_DMR)
// The mode a VFO starts in
#define VFO_MODE_DEFAULT	(CONFIG_VFO_FM ? RADIO_MODE_FM : RADIO_MODE_DMR)

static const uint32_t vfo_start_hz[] = { 146520000, 145500000, 446000000, 433500000 };

// The VFO's step in use: auto follows the width (the air band's channels are 25 kHz apart); in the broadcast band
// it is the broadcast step setting's
static void step_update(radio_sm_t *sm)
{
	uint32_t hz = sm->vfo_step_set;
	if (hz == 0)
		hz = sm->view.ch.bw == RADIO_BW_25K || sm->view.am ? 25000u : RADIO_VFO_STEP_HZ;
	sm->view.vfo_step_hz = hz;
	sm->view.vfo_step_auto = sm->vfo_step_set == 0;
}

static void changed(radio_sm_t *sm)
{
	step_update(sm);	// every change that can move the width comes here
	sm->serial++;
}

void radio_settings_default(radio_settings_t *s)
{
	memset(s, 0, sizeof(*s));
	s->tot_s = RADIO_TOT_DEFAULT_S;
	s->tot_warn_s = 10;
	s->bcl = RADIO_BCL_OFF;
	s->scan_resume = SCAN_RESUME_CARRIER;
	s->scan_hold_s = RADIO_SCAN_HOLD_S;
	s->scan_delay_ms = RADIO_SCAN_DELAY_MS;
	s->scan_dwell_ms = RADIO_SCAN_DWELL_MS;
	s->dual_delay_ms = RADIO_DUAL_DELAY_MS;
	s->beep_level = 50;
	s->key_beep = true;
	s->talk_permit = true;
	s->vox = RADIO_VOX_OFF;
	s->vox_level = 5;
	s->vox_tail_ms = 1500;
	s->rx_save = 0;		// off until the board's receiver sleep is verified on the bench
	s->rx_save_idle_s = 10;
	s->battery_off_s = 0;
	s->backlight_dim_s = 30;
	s->backlight_off_s = 120;
	s->reply_s = RADIO_REPLY_CODEPLUG;
	s->roam_rssi_dbm = -105;	// ROAM_RSSI_DEFAULT
	s->roam_hyst_db = 6;		// ROAM_HYST_DEFAULT
	s->roam_probe = 0;		// passive: no automatic transmission (P7)
	s->roam_check_s = 0;
	s->roam_beacon_s = 60;		// ROAM_BEACON_DEFAULT_S: the common setting of networked repeaters
	s->roam_beacon_len_s = 4;	// ROAM_BEACON_LEN_DEFAULT
	s->roam_survey = 1;		// listening only (P7 allows it): the survey of the synchronized beacon window
	s->rpt_beacon = false;		// transmits: off until the user turns it on (P7)
	s->sfr_rx = 0;			// DMR_SFR_RX_REPEATER
	s->sfr_cwid = 0;		// transmits: off until the user turns it on
}

static void activity(radio_sm_t *sm, uint32_t now_ms)
{
	sm->activity_ms = now_ms;
}

// A beep at the settings' level: mixed into the DMR voice stream when it runs, else on its own stream
static void beep(radio_sm_t *sm, uint8_t b)
{
#if CONFIG_BEEP
	bool shared = CONFIG_M17 && sm->dv_running;
	uint8_t level = sm->set.beep_level;
	if (board.radio != NULL && board.radio->volume != NULL && !board.radio->volume_pcm) {
		level = radio_beep_level(level, sm->volume_sent);
	}
	beep_play((beep_t)b, level, shared);
#else
	(void)sm;
	(void)b;
#endif
}

// The TX time-out in effect (FM and DMR): the channel's own from the codeplug, or the radio's (settings, 180 s by
// default) when the channel's is 0 (builtin channels and the VFO too)
static void apply_tot(radio_sm_t *sm)
{
	uint16_t t = sm->view.ch.tot_s;
	sm->view.tot_s = t != 0 ? t : sm->set.tot_s;
}

static uint8_t master_power = SETTINGS_UNSET8;	// the settings' power for "default" channels
// The radio task's copy of the settings for the few places that read more than one field (data_init, cw_begin,
// reload). Static: off the radio task's stack; one copy, since none of them calls another (RAM is tight on some radios)
static settings_t st_copy;

static uint8_t default_power(void)
{
	if (board.power_count == 0) {
		return 0;
	}
	if (master_power < board.power_count) {
		return master_power;
	}
	return board.power_default < board.power_count ? board.power_default : board.power_count - 1;
}

static uint8_t channel_power(const channel_t *ch)
{
	if (ch->power == CODEPLUG_POWER_DEFAULT || ch->power >= board.power_count) {
		return default_power();
	}
	return ch->power;
}

uint8_t radio_sm_squelch_open_below(uint8_t level)
{
	if (level == 0) {
		return 255;
	}
	if (level > RADIO_SQUELCH_MAX) {
		level = RADIO_SQUELCH_MAX;
	}
	return (uint8_t)(RADIO_SQL_OPEN_L1 - (level - 1) * RADIO_SQL_DB_PER_STEP);
}

bool radio_view_wakes(const radio_view_t *v)
{
	if (v->hotspot) {
		return false;		// the host's traffic
	}
	switch (v->state) {
	case RS_TX:
		return !v->tx_auto;
	case RS_TX_DENIED:
		return true;		// PTT held: the screen says why
	case RS_RX:
		if (v->bcast) {
			return false;	// a broadcast program plays on: not something new to look at
		}
		return true;		// FM: the squelch opened, something plays
	default:
		return false;
	}
}

static bool same_tone(const subtone_t *a, const subtone_t *b)
{
	return a->type == b->type && (a->type == TONE_NONE || a->value == b->value);
}

// Recompute the temporary flags. Bandwidth and tones count against the channel's own values; in the VFO they are
// the VFO's settings, never temporary
static void set_temporary(radio_sm_t *sm)
{
	uint8_t t = 0;
	if (sm->temp_power) {
		t |= RADIO_TEMP_POWER;
	}
	if (sm->temp_squelch) {
		t |= RADIO_TEMP_SQUELCH;
	}
	if (!sm->view.vfo) {
		if (sm->view.ch.bw != sm->base_bw) {
			t |= RADIO_TEMP_BW;
		}
		if (!same_tone(&sm->view.ch.tx_tone, &sm->base_tx_tone)) {
			t |= RADIO_TEMP_TX_TONE;
		}
		if (!same_tone(&sm->view.ch.rx_tone, &sm->base_rx_tone)) {
			t |= RADIO_TEMP_RX_TONE;
		}
		if (sm->view.ch.mode != sm->base_mode) {
			t |= RADIO_TEMP_MODE;
		}
		if (sm->view.ch.mode == RADIO_MODE_DMR) {
			if (sm->view.ch.color_code != sm->base_cc) {
				t |= RADIO_TEMP_CC;
			}
			if (sm->temp_ts != 0) {
				t |= RADIO_TEMP_TS;
			}
		}
	}
	sm->view.temp = t;
	sm->view.temporary = t != 0;
}

// Back to the channel's own settings. Returns whether the receiver needs retuning (bandwidth, RX tone, mode, color
// code or timeslot changed: tune() also reconfigures the DMR call stack)
static bool clear_temporary(radio_sm_t *sm)
{
	bool retune = (sm->view.temp & (RADIO_TEMP_BW | RADIO_TEMP_RX_TONE | RADIO_TEMP_MODE | RADIO_TEMP_CC |
					RADIO_TEMP_TS)) != 0;
	sm->temp_power = false;
	sm->temp_squelch = false;
	sm->temp_ts = 0;
	sm->ts_pending = 0;
	sm->view.power = channel_power(&sm->view.ch);
	sm->view.squelch = sm->base_squelch;
	if (!sm->view.vfo) {
		sm->view.ch.bw = sm->base_bw;
		sm->view.ch.rx_tone = sm->base_rx_tone;
		sm->view.ch.tx_tone = sm->base_tx_tone;
		sm->view.ch.mode = sm->base_mode;
		sm->view.ch.color_code = sm->base_cc;
		sm->view.ch.timeslot = sm->base_ts;
	}
	set_temporary(sm);
	return retune;
}

// RX tone squelch: whether the tone counts as heard, debounced. Always heard when the channel sets no RX tone
// (DMR channels never use one)
static bool tone_needed(const radio_sm_t *sm)
{
	if (sm->view.am || sm->view.bcast) {
		return false;	// AM and broadcast: sub-audio ignored
	}
	return sm->view.ch.rx_tone.type != TONE_NONE && sm->view.ch.mode == RADIO_MODE_FM;
}

static void follow_tone(radio_sm_t *sm, bool raw)
{
	bool needed = tone_needed(sm);
	if (!needed) {
		raw = true;
	}
	if (raw == sm->view.tone_ok) {
		sm->tone_count = 0;
		return;
	}
	sm->tone_count++;
	if (!needed || sm->tone_count >= (raw ? RADIO_TONE_ON_POLLS : RADIO_TONE_OFF_POLLS)) {
		sm->view.tone_ok = raw;
		sm->tone_count = 0;
		changed(sm);
	}
}

// After a retune, or with a new RX tone: nothing heard yet (or no tone needed)
static void reset_tone(radio_sm_t *sm)
{
	sm->view.tone_ok = !tone_needed(sm);
	sm->tone_count = 0;
}

// The APRS job's phases (aprs_phase)
enum { APRS_RF_OFF = 0, APRS_RF_LISTEN, APRS_RF_KEYUP, APRS_RF_SEND };

// A broadcast station is listened to, not squelched: the state stays RX (so the screen shows the level and the LED is on)
// whatever the signal, and the audio plays. A scan keeps the squelch to stop on stations
static bool bcast_listening(const radio_sm_t *sm)
{
	(void)sm;
	return false;
}

// Speaker and LEDs follow the state; only changes go to the board
// FM: the board's speaker path follows the squelch (or the monitor). DMR voice goes out as PCM from the vocoder
// (core/dmr/ambe routes the speaker itself); the green LED shows a call being played
static void outputs(radio_sm_t *sm)
{
	bool tx = sm->view.state == RS_TX;
	bool fm = sm->view.ch.mode == RADIO_MODE_FM && !sm->hotspot;
	bool open = !tx && fm && (sm->view.monitor || sm->view.state == RS_RX);
	bool green = open;
#if RADIO_SM_DV
	if (sm->dv_running && !tx && sm->view.state == RS_RX) {
		green = true;		// a P25 call plays (its PCM goes out from the native session's worker)
	}
#endif
#if RADIO_SM_CW
	if (sm->cw_phase != 0) {
		tx = true;	// a CW ID
	}
#endif

	// The broadcast receiver plays instead of the channel receiver while the channel is a broadcast one
	bool bcast = false;
	if (sm->audio_out != (int8_t)(open && !bcast)) {
		sm->audio_out = (int8_t)(open && !bcast);
		board.radio->audio(open && !bcast);
	}
	if (sm->green_out != (int8_t)green) {
		sm->green_out = (int8_t)green;
		board.power->led(LED_GREEN, green);
	}
#if CONFIG_DUAL_WATCH
	// The second receiver has the speaker while the selected channel is quiet: that one's calls always win
	if (board.rx2 != NULL) {
		int8_t on = (int8_t)(sm->rx2_on && !tx && !green);
		if (sm->rx2_audio_out != on) {
			sm->rx2_audio_out = on;
			board.rx2->audio(on != 0);
		}
	}
#endif
	if (sm->red_out != (int8_t)tx) {
		sm->red_out = (int8_t)tx;
		board.power->led(LED_RED, tx);
	}
	sm->view.leds = (uint8_t)((green ? RADIO_LED_GREEN : 0u) | (tx ? RADIO_LED_RED : 0u));
}

static void end_reception(radio_sm_t *sm, uint32_t now_ms)
{
	if (sm->view.state == RS_RX) {
		sm->view.state = RS_IDLE;
		sm->view.last_rx_ms = now_ms != 0 ? now_ms : 1;
	}
	sm->close_count = 0;
}

// Receive on what view.ch says now
static void dmr_begin(radio_sm_t *sm);
static void dmr_end(radio_sm_t *sm);
#if RADIO_SM_DV
static void dv_begin(radio_sm_t *sm);
static void dv_end(radio_sm_t *sm);
#endif


// The air band (on a radio with AM receive) and the broadcast receiver's range: receive only, and never DMR
// (docs/spec/airband-and-broadcast.md 2)
static bool listen_band(uint32_t hz)
{
	(void)hz;
	return false;
}

static void tune(radio_sm_t *sm, uint32_t now_ms)
{
	sm->retune_pending = sm->retune_due = false;	// this tune is the one they wait for
	end_reception(sm, now_ms);
	if (listen_band(sm->view.ch.rx_hz)) {
		// AM or broadcast there, whatever the channel says; the VFO stays FM when it leaves the band. The channel's
		// own mode follows, so FM there is not a temporary change (dual watch brings a pane back already FM)
		sm->view.ch.mode = RADIO_MODE_FM;
		if (sm->view.vfo)
			sm->vfo_ch.mode = RADIO_MODE_FM;
		else
			sm->base_mode = RADIO_MODE_FM;
		set_temporary(sm);
	}
	reset_tone(sm);
#if CONFIG_BATTERY_SAVE
	sm->save_on = sm->save_asleep = false;	// rx() below wakes the receiver
	sm->view.rx_sleeping = false;
#endif
	radio_mode_t mode = (CONFIG_DMR && sm->view.ch.mode == RADIO_MODE_DMR) ? RADIO_MODE_DMR : RADIO_MODE_FM;
	// No sub-audio squelch in DMR, whatever tone the channel or the VFO still holds from its FM settings
	static const subtone_t no_tone = { TONE_NONE, 0 };
#if RADIO_SM_DV
	bool dv = codeplug_mode_digital_voice(sm->view.ch.mode);
	if (sm->dv_running && (!dv || sm->view.am || sm->view.bcast)) {
		dv_end(sm);		// the native session lets go: the board is in FM again
	}
#endif
	if (sm->board_mode == RADIO_MODE_DMR && mode != RADIO_MODE_DMR) {
		dmr_end(sm);		// the call stack lets go of the modem before the chip leaves DMR
	}
	if (mode != sm->board_mode) {
		sm->board_mode = mode;
		board.radio->set_mode(mode);
	}
#if RADIO_SM_DV
	if (dv && !sm->view.am && !sm->view.bcast) {
		// The native session tunes the receiver itself (from FM, which the board is in now)
		dv_begin(sm);
		outputs(sm);
		changed(sm);
		return;
	}
#endif
	if (!sm->view.am && !sm->view.bcast)
		board.radio->rx(sm->view.ch.rx_hz, (radio_bw_t)sm->view.ch.bw,
				mode == RADIO_MODE_DMR ? &no_tone : &sm->view.ch.rx_tone);
	if (mode == RADIO_MODE_DMR) {
		dmr_begin(sm);		// after set_mode(DMR): the modem only starts in DMR mode
	}
	outputs(sm);
	changed(sm);
}

// From position start in z, the first readable channel going in dir (wraps once round the zone)
static bool find_channel(const zone_t *z, int start, int dir, uint16_t *pos, channel_t *ch)
{
	int n = z->count;
	for (int i = 0; i < n; i++) {
		int p = ((start + dir * i) % n + n) % n;
		if (codeplug_channel(z->channels[p], ch)) {
			*pos = (uint16_t)p;
			return true;
		}
	}
	return false;
}

static void set_zone(radio_sm_t *sm, uint16_t index, const zone_t *z)
{
	if (z != &sm->zone) {
		sm->zone = *z;
	}
	sm->zone.name[CODEPLUG_NAME_LEN] = '\0';
	sm->view.zone_index = index;
	sm->view.zone_size = sm->zone.count;
	memcpy(sm->view.zone_name, sm->zone.name, sizeof(sm->view.zone_name));
}

// A channel change: the channel's own settings, temporary changes dropped (P7)
static void set_channel(radio_sm_t *sm, uint16_t pos, const channel_t *ch)
{
	sm->view.ch = *ch;
	sm->view.ch.name[CODEPLUG_NAME_LEN] = '\0';
#if !CONFIG_SUBTONE
	sm->view.ch.rx_tone = sm->view.ch.tx_tone = (subtone_t){ TONE_NONE, 0 };	// built without CTCSS / DCS
#endif
	sm->view.vfo = false;
	sm->view.zone_pos = pos;
	sm->view.channel_number = sm->zone.channels[pos];
	sm->base_bw = sm->view.ch.bw;
	sm->base_rx_tone = sm->view.ch.rx_tone;
	sm->base_tx_tone = sm->view.ch.tx_tone;
	sm->base_mode = sm->view.ch.mode;
	sm->base_cc = sm->view.ch.color_code;
	sm->base_ts = sm->view.ch.timeslot;
	clear_temporary(sm);
	apply_tot(sm);
}

static bool read_zone(uint16_t index, zone_t *z)
{
	if (!codeplug_zone(index, z)) {
		return false;
	}
	if (z->count > CODEPLUG_ZONE_MAX) {
		z->count = CODEPLUG_ZONE_MAX;
	}
	return z->count > 0;
}

static uint32_t vfo_default_hz(void)
{
	for (unsigned i = 0; i < sizeof(vfo_start_hz) / sizeof(vfo_start_hz[0]); i++) {
		if (board_in_band(vfo_start_hz[i], true)) {
			return vfo_start_hz[i];
		}
	}
	for (uint8_t i = 0; i < board.band_count; i++) {
		if (board.bands[i].tx) {
			uint32_t mid = board.bands[i].lo_hz + (board.bands[i].hi_hz - board.bands[i].lo_hz) / 2;
			return mid - mid % RADIO_VFO_STEP_HZ;
		}
	}
	return board.band_count > 0 ? board.bands[0].lo_hz : 0;
}

// A new RX frequency. A split VFO keeps its offset within one transmit band (board_offset_carries); otherwise it is
// simplex
static void vfo_set_hz(radio_sm_t *sm, uint32_t hz)
{
	uint32_t tx = sm->vfo_ch.tx_hz - sm->vfo_ch.rx_hz + hz;
	if (!sm->vfo_seeded || !board_offset_carries(sm->vfo_ch.rx_hz, hz, tx))
		tx = hz;
	sm->vfo_ch.rx_hz = hz;
	sm->vfo_ch.tx_hz = tx;
	sm->vfo_seeded = true;
	if (sm->view.vfo) {
		sm->view.ch.rx_hz = hz;
		sm->view.ch.tx_hz = tx;
	}
}

static void enter_vfo(radio_sm_t *sm, uint32_t now_ms)
{
	if (!sm->vfo_seeded) {
		bool keep = !sm->view.vfo && board_in_band(sm->view.ch.rx_hz, false);
		uint32_t hz = keep ? sm->view.ch.rx_hz : vfo_default_hz();
		vfo_set_hz(sm, hz);
	}
	sm->view.ch = sm->vfo_ch;
	sm->view.vfo = true;
	sm->view.channel_number = 0;
	clear_temporary(sm);
	apply_tot(sm);
	tune(sm, now_ms);
}

#if CONFIG_DUAL_WATCH
// VFO idx (0 or 1) becomes the one in use: vfo_ch and its talk target swap with vfo_other. The caller retunes when
// the radio is in the VFO
static void vfo_use(radio_sm_t *sm, uint8_t idx)
{
	idx &= 1u;
	if (idx == sm->vfo_idx) {
		return;
	}
	radio_vfo_t t = sm->vfo_other;
	sm->vfo_other.ch = sm->vfo_ch;
	sm->vfo_other.seeded = sm->vfo_seeded;
	sm->vfo_other.talk_id = sm->vfo_talk_id;
	sm->vfo_other.talk_private = sm->vfo_talk_private;
	sm->vfo_ch = t.ch;
	sm->vfo_seeded = t.seeded;
	sm->vfo_talk_id = t.talk_id;
	sm->vfo_talk_private = t.talk_private;
	sm->vfo_idx = idx;
	sm->view.vfo_index = idx;
}
#endif

radio_vfo_copy_t radio_vfo_copy_check(const channel_t *ch, bool in_vfo)
{
	if (!VFO_AVAILABLE) {
		return VFO_COPY_NO_VFO;
	}
	if (in_vfo) {
		return VFO_COPY_ALREADY;
	}
	if (!radio_vfo_mode_built(ch->mode)) {
		return VFO_COPY_NO_MODE;
	}
	if (!board_in_band(ch->rx_hz, false) || (ch->tx_hz != 0 && !board_in_band(ch->tx_hz, false))) {
		return VFO_COPY_OUT_OF_BAND;
	}
	return VFO_COPY_OK;
}

// Copy to VFO: the channel as it is in effect now (view.ch holds the temporary bandwidth, tones, mode, color code and
// timeslot; the temporary power is view.power; a talk target chosen for now counts, a one-shot or reply target does
// not) becomes the VFO's own settings, which are saved like any VFO change. Not copied, the VFO has nothing to hold
// them: the RX group list, the privacy key, roaming, data and emergency settings, the squelch level
static void vfo_copy(radio_sm_t *sm, uint32_t now_ms)
{
	const channel_t *c = &sm->view.ch;
	if (radio_vfo_copy_check(c, sm->view.vfo) != VFO_COPY_OK) {
		return;
	}
	channel_t *v = &sm->vfo_ch;
	v->rx_hz = c->rx_hz;
	v->tx_hz = c->tx_hz != 0 && !c->dmo ? c->tx_hz : c->rx_hz;
	v->mode = c->mode;
	v->bw = c->bw;
	v->power = sm->temp_power ? sm->view.power : c->power;
	v->nac_rx = c->nac_rx;
	v->nac_tx = c->nac_tx;
	v->talkgroup = c->talkgroup;
	if (c->mode == RADIO_MODE_DMR) {
	} else {
		v->rx_tone = c->rx_tone;	// a DMR channel has no tones: the VFO keeps its own for its FM mode
		v->tx_tone = c->tx_tone;
	}
	sm->vfo_seeded = true;
	enter_vfo(sm, now_ms);
}

static void leave_vfo(radio_sm_t *sm, uint32_t now_ms)
{
	uint16_t pos;
	channel_t ch;
	if (!sm->have_channels || !find_channel(&sm->zone, sm->view.zone_pos, 1, &pos, &ch)) {
		return;
	}
	set_channel(sm, pos, &ch);
	tune(sm, now_ms);
}

// Next band edge past hz in dir, wrapping round the board's bands
static uint32_t band_wrap(uint32_t hz, int dir)
{
	bool found = false;
	uint32_t best = 0;
	for (uint8_t i = 0; i < board.band_count; i++) {
		const band_t *b = &board.bands[i];
		if (dir > 0 && b->lo_hz > hz && (!found || b->lo_hz < best)) {
			best = b->lo_hz;
			found = true;
		} else if (dir < 0 && b->hi_hz < hz && (!found || b->hi_hz > best)) {
			best = b->hi_hz;
			found = true;
		}
	}
	if (found) {
		return best;
	}
	for (uint8_t i = 0; i < board.band_count; i++) {
		const band_t *b = &board.bands[i];
		uint32_t edge = dir > 0 ? b->lo_hz : b->hi_hz;
		if (!found || (dir > 0 ? edge < best : edge > best)) {
			best = edge;
			found = true;
		}
	}
	return best;
}

static void vfo_step(radio_sm_t *sm, int dir, uint32_t now_ms)
{
	step_update(sm);
	int64_t hz = (int64_t)sm->view.ch.rx_hz + (int64_t)dir * sm->view.vfo_step_hz;
	uint32_t next;
	if (hz > 0 && hz <= (int64_t)UINT32_MAX && board_in_band((uint32_t)hz, false)) {
		next = (uint32_t)hz;
	} else {
		next = band_wrap(sm->view.ch.rx_hz, dir);
	}
	vfo_set_hz(sm, next);
	tune(sm, now_ms);
}

static void channel_step(radio_sm_t *sm, int dir, uint32_t now_ms)
{
	uint16_t pos;
	channel_t ch;
	if (!find_channel(&sm->zone, sm->view.zone_pos + dir, dir, &pos, &ch)) {
		return;
	}
	set_channel(sm, pos, &ch);
	tune(sm, now_ms);
}

// Something on the channel by the noise reading, at the squelch level (the default level when the squelch is open)
static bool carrier_heard(const radio_sm_t *sm, uint8_t noise)
{
	uint8_t sq = sm->view.squelch != 0 ? sm->view.squelch : RADIO_SQUELCH_DEFAULT;
	return noise < radio_sm_squelch_open_below(sq);
}

#if RADIO_SM_DV
// --- Digital voice channels (P25, NXDN): radio_dv.h keeps the modem; the radio core keeps the policy ---

// The unit ID our calls carry: the channel's ID override, else the radio's DMR ID
static uint32_t dv_unit_id(const channel_t *ch)
{
	(void)ch;return 0;
}

#define NXDN_UNIT_MAX	65519u	// 65520-65535 are special IDs (65535 the all call)

// The digital voice modes this build has
static bool dv_mode_built(uint8_t mode)
{
	return (mode == RADIO_MODE_P25 && CONFIG_P25 && CONFIG_P25_NATIVE) || (mode == RADIO_MODE_M17 && CONFIG_M17) ||
	       (mode == RADIO_MODE_P25_TRUNK && CONFIG_P25_TRUNK);
}

static void dv_begin(radio_sm_t *sm)
{
	const channel_t *ch = &sm->view.ch;
	if (sm->dv_monitor_latch && (sm->dv_latch_vfo != sm->view.vfo || sm->dv_latch_ch != sm->view.channel_number)) {
		sm->dv_monitor_latch = false;	// digital monitor lasts as long as its channel, as on DMR

	}
	radio_dv_cfg_t c = {
		.mode = ch->mode, .rx_hz = ch->rx_hz, .tx_hz = ch->tx_hz, .power = sm->view.power,
		.nac_rx = ch->nac_rx, .nac_tx = ch->nac_tx, .talkgroup = ch->talkgroup, .unit_id = dv_unit_id(ch),
		.hear_all = ch->hear_all, .wacn = ch->wacn, .sysid = ch->sysid,
	};
	if (sm->dv_monitor_latch) {
		// Digital monitor (the monitor key's press, as on DMR): every group, and on a conventional channel every NAC
		// (P25) or RAN (NXDN RX RAN 0). TX keeps the channel's NAC / RAN and group; a trunk keeps its system's NAC
		c.hear_all = true;
		if (ch->mode == RADIO_MODE_P25)
			c.nac_rx = RADIO_DV_NAC_ANY;
		else if (ch->mode == RADIO_MODE_M17)
			c.nac_rx = 0xFFFF;
	}
	sm->dv_running = radio_dv_select(&c);
	memset(&sm->view.dv, 0, sizeof(sm->view.dv));
	sm->view.dv.active = sm->dv_running;
	sm->view.dv.built = dv_mode_built(ch->mode);
}

static void dv_end(radio_sm_t *sm)
{
	if (sm->dv_running) {
		radio_dv_select(NULL);	// the board is back in FM when this returns
		sm->dv_running = false;
		sm->board_mode = RADIO_MODE_FM;
	}
	sm->dv_permit_ms = 0;
	sm->view.dv.active = false;
	sm->view.dv.state = RDV_IDLE;
}
#endif

// Busy channel lockout (FM): refuse while the channel is in use (by anyone, or by a group with another tone)
static bool bcl_busy(const radio_sm_t *sm)
{
#if RADIO_SM_DV
	// P25 / NXDN: CARRIER refuses while frames of any NAC (RAN) are heard; WRONG_TONE ("NAC") while frames of another
	// NAC (RAN) are, the channel used by a group we don't hear (as an FM carrier without our tone). Our own NAC's (RAN's)
	// call: talk back
	// Trunking: the system hands out the channels, nothing to lock out
	if (sm->dv_running && sm->set.bcl != RADIO_BCL_OFF && sm->view.ch.mode != RADIO_MODE_P25_TRUNK) {
		radio_dv_status_t s;
		radio_dv_status(&s, sm->dv_now_ms);
		return sm->set.bcl == RADIO_BCL_CARRIER ? s.sync : (s.sync && !s.sync_ours);
	}
#endif
	if (sm->view.ch.mode != RADIO_MODE_FM || sm->set.bcl == RADIO_BCL_OFF) {
		return false;
	}
	bool carrier = sm->view.state == RS_RX || carrier_heard(sm, sm->view.noise);
	if (sm->set.bcl == RADIO_BCL_CARRIER) {
		return carrier;
	}
	return carrier && tone_needed(sm) && !sm->view.tone_ok;
}

static uint8_t tx_check(const radio_sm_t *sm)
{
	const channel_t *ch = &sm->view.ch;
	if (sm->set.tx_inhibit) {
		return TXDENY_INHIBIT;
	}
	if (ch->tx_hz == 0) {
		return TXDENY_NO_FREQ;
	}
	if (ch->rx_only) {
		return TXDENY_RX_ONLY;
	}
	if (ch->mode == RADIO_MODE_DMR ? !CONFIG_DMR : ch->mode == RADIO_MODE_FM ? !CONFIG_FM : !RADIO_SM_DV) {
		return TXDENY_MODE;
	}
#if RADIO_SM_DV
	if (codeplug_mode_digital_voice(ch->mode)) {
		if (!dv_mode_built(ch->mode)) {
			return TXDENY_MODE;		// the channel's mode is not built in
		}
		if (!sm->dv_running) {
			return TXDENY_NO_DMR;	// the digital session is not up (starting or recovering): the modem
		}
		if (ch->mode == RADIO_MODE_P25_TRUNK && !CONFIG_P25_TRUNK_TX) {
			return TXDENY_MODE;		// trunked transmit is a separate switch, off by default (receive only)
		}
		if (ch->mode == RADIO_MODE_M17 && !radio_m17_identity_valid())return TXDENY_NO_ID;
		if (ch->mode != RADIO_MODE_M17 && (dv_unit_id(ch) == 0 || ch->talkgroup == 0)) {
			return TXDENY_NO_ID;		// a P25 call needs our unit ID and a talkgroup (TIA-102.BAAD-B 6.1.1.1)
		}
		if (ch->mode == RADIO_MODE_P25_TRUNK && (ch->wacn == 0 || ch->sysid == 0)) {
			return TXDENY_NO_ID;		// and a trunked one the system's WACN and System ID, as the user entered them
		}
		if (false) {
			return TXDENY_NO_ID;		// NXDN unit IDs are 16 bits, 1-65519: set the channel's own ID
		}
	}
#endif
	if (!board_in_band(ch->tx_hz, true)) {
		return TXDENY_BAND;
	}
	if (sm->batt.level == BATT_CRITICAL) {
		return TXDENY_BATTERY;
	}
	if (bcl_busy(sm)) {
		return TXDENY_BUSY;
	}
	return TXDENY_NONE;
}

static void deny(radio_sm_t *sm, uint8_t reason)
{
	sm->view.state = RS_TX_DENIED;
	sm->view.tx_deny = reason;
	sm->view.tot_warning = false;
	outputs(sm);
	changed(sm);
	beep(sm, BEEP_TX_DENY);
}

// The TX timer in the view, and the warning tot_warn_s before the time-out (FM and DMR)
static void tx_timer(radio_sm_t *sm, uint32_t now_ms)
{
	uint32_t elapsed = now_ms - sm->tx_start_ms;
	if (elapsed / 1000 != sm->view.tx_elapsed_ms / 1000) {
		changed(sm);
	}
	sm->view.tx_elapsed_ms = elapsed;
	uint16_t tot = sm->view.tot_s, warn = sm->set.tot_warn_s;
	if (tot != 0 && warn != 0 && tot > warn && !sm->tot_warned && elapsed >= (uint32_t)(tot - warn) * 1000u) {
		sm->tot_warned = true;
		sm->view.tot_warning = true;
		changed(sm);
		beep(sm, BEEP_TOT_WARN);
	}
}

#if RADIO_SM_DV
// The talk permit before keying a digital voice channel (BEEP_TX_PERMIT_DV, Motorola style): the beep plays while the
// channel still receives, then the session keys. The hold also waits for the beep's own stream to stop: the transmit
// session starts the microphone's stream, and beep_service would stop that one under it
#define DV_PERMIT_MAX_MS	600u	// keys after this long whatever the beep says

// Not on trunked channels: there the permit means the system granted the call, not that PTT was pressed
static bool dv_permit_on(const radio_sm_t *sm)
{
	return CONFIG_BEEP && sm->set.talk_permit && !sm->view.ch.no_beep && sm->view.ch.mode != RADIO_MODE_P25_TRUNK;
}

static bool dv_permit_busy(void)
{
#if CONFIG_BEEP
	const hal_audio_t *a = board.audio;
	return beep_busy() || beep_streaming() || (a != NULL && a->spk_pending != NULL && a->spk_pending());
#else
	return false;
#endif
}

// The TX rules passed (tx_check) and the talk permit (if any) played: the channel's session switches to transmit
static void dv_key(radio_sm_t *sm, uint32_t now_ms)
{
	radio_dv_status_t s;
	radio_dv_status(&s, now_ms);
	if (memcmp(sm->view.dv.callsign,s.callsign,10)||memcmp(sm->view.dv.destination,s.destination,10)){memcpy(sm->view.dv.callsign,s.callsign,10);memcpy(sm->view.dv.destination,s.destination,10);changed(sm);}
	if (!radio_dv_ptt(true)) {
		deny(sm, TXDENY_NO_DMR);
		return;
	}
	sm->dv_tx_calls = s.tx_calls;
	sm->dv_tot = false;
	sm->view.state = RS_TX;
	sm->view.tx_auto = false;
	sm->view.last_tx_ms = now_ms != 0 ? now_ms : 1;
	sm->view.tx_deny = TXDENY_NONE;
	sm->view.tx_elapsed_ms = 0;
	sm->tx_start_ms = now_ms;
	sm->view.dv.state = RDV_TX;
	sm->view.dv.nac = sm->view.ch.nac_tx;
	sm->view.dv.talkgroup = sm->view.ch.talkgroup;
	sm->view.dv.source = dv_unit_id(&sm->view.ch);
	sm->view.dv.encrypted = sm->view.dv.emergency = false;
	sm->view.dv.call_ms = now_ms;
	outputs(sm);
	changed(sm);
}

// PTT on a digital voice channel: the talk permit first, if it is on
static void dv_ptt_down(radio_sm_t *sm, uint32_t now_ms)
{
	if (!dv_permit_on(sm)) {
		dv_key(sm, now_ms);
		return;
	}
	beep(sm, BEEP_TX_PERMIT_DV);
	sm->dv_permit_ms = now_ms != 0 ? now_ms : 1;
}

// Every tick: key once the talk permit is over (PTT let go during it: ptt_release dropped it)
static void dv_permit_poll(radio_sm_t *sm, uint32_t now_ms)
{
	if (sm->dv_permit_ms == 0 || (dv_permit_busy() && now_ms - sm->dv_permit_ms < DV_PERMIT_MAX_MS)) {
		return;
	}
	sm->dv_permit_ms = 0;
	dv_key(sm, now_ms);
}

// The native session's state into the view: calls heard, the end of our transmission, its time-out
static void dv_follow(radio_sm_t *sm, uint32_t now_ms)
{
	radio_dv_status_t s;
	radio_dv_view_t *v = &sm->view.dv;
	radio_dv_status(&s, now_ms);
	if(memcmp(v->callsign,s.callsign,10)||memcmp(v->destination,s.destination,10)){memcpy(v->callsign,s.callsign,10);memcpy(v->destination,s.destination,10);changed(sm);}

	if (s.active != v->active || s.fault != v->fault) {
		v->active = s.active;
		v->fault = s.fault;
		changed(sm);
	}
	if (sm->view.ch.mode == RADIO_MODE_P25_TRUNK && (s.trunk_state != v->trunk || s.trunk_registered != v->trunk_registered)) {
		v->trunk = s.trunk_state;
		v->trunk_registered = s.trunk_registered;
		changed(sm);
	}
	if (sm->view.state == RS_TX) {
		tx_timer(sm, now_ms);
		if (!sm->dv_tot && sm->view.tot_s != 0 && sm->view.tx_elapsed_ms >= (uint32_t)sm->view.tot_s * 1000u) {
			sm->dv_tot = true;
			radio_dv_ptt(false);	// the terminator goes out, then the PA drops
		}
		bool ended = s.tx_calls != sm->dv_tx_calls;
		if (!ended && (sm->ptt || s.transmitting)) {
			return;			// still on air, or still to start
		}
		uint8_t why = sm->dv_tot ? TXDENY_TIMEOUT : ended ? radio_dv_tx_deny(s.tx_reason) : TXDENY_NONE;
		v->state = RDV_IDLE;
		sm->tx_end_ms = now_ms;
		if (why != TXDENY_NONE && sm->ptt) {
			deny(sm, why);		// shown until PTT is released
			return;
		}
		sm->view.state = RS_IDLE;
		sm->view.tot_warning = false;
		outputs(sm);
		changed(sm);
		return;
	}
	if (sm->view.state == RS_TX_DENIED) {
		return;
	}
	bool rx = s.playing;
	uint8_t st = rx ? RDV_RX : s.sync ? RDV_RX_OTHER : RDV_IDLE;
	if (rx && sm->view.state != RS_RX) {
		sm->view.state = RS_RX;
		v->call_ms = now_ms;
		changed(sm);
	} else if (!rx && sm->view.state == RS_RX) {
		end_reception(sm, now_ms);
		v->last_talkgroup = v->talkgroup;
		v->last_source = v->source;
		v->last_ms = now_ms != 0 ? now_ms : 1;
		changed(sm);
	}
	if (rx) {
		activity(sm, now_ms);
	}
	if (st != v->state || (s.sync && (s.nac != v->nac || s.talkgroup != v->talkgroup || s.source != v->source ||
			s.encrypted != v->encrypted || s.emergency != v->emergency))) {
		v->state = st;
		if (s.sync) {
			v->nac = s.nac;
			v->talkgroup = s.talkgroup;
			v->source = s.source;
			v->encrypted = s.encrypted;
			v->emergency = s.emergency;
		}
		changed(sm);
	}
	outputs(sm);
}
#endif

// --- DMR: the call stack on DMR channels (core/dmr/call), voice through core/dmr/ambe ---
static void dmr_begin(radio_sm_t *sm) { (void)sm; }
static void dmr_end(radio_sm_t *sm) { (void)sm; }

// --- Scan (radio/scan.c decides; this tunes) ---
#if CONFIG_SCAN
static bool scan_usable(uint16_t pos, void *ctx)
{
	const radio_sm_t *sm = ctx;
	channel_t ch;
	return pos < sm->zone.count && codeplug_channel(sm->zone.channels[pos], &ch) && !ch.scan_skip;
}

// Dual watch (SCAN_DUAL): the receiver alternates between two channels, dual[0] and dual[1], skip flags ignored. With
// CONFIG_DUAL_WATCH each is a channel of any zone or the VFO, and PTT talks on the selected one without ending the
// watch; without it, two channels of the current zone (the debug link's) and PTT ends it as any scan

static bool dual_running(const radio_sm_t *sm)
{
	return sm->scan.kind == SCAN_DUAL;
}

static void dual_busy_set(radio_sm_t *sm, uint8_t busy)
{
	if (sm->view.dual_busy != busy) {
		sm->view.dual_busy = busy;
		changed(sm);
	}
}

// The receiver is on the other channel for a look (dual_wait_service): its squelch stays shut
static bool dual_peeking(const radio_sm_t *sm)
{
#if CONFIG_DUAL_WATCH
	return sm->dual_peek && dual_running(sm);
#else
	(void)sm;
	return false;
#endif
}

static bool dual_same(const radio_dual_ref_t *a, const radio_dual_ref_t *b)
{
	return a->vfo == b->vfo && a->pos == b->pos && (a->vfo || a->zone == b->zone);
}

// The receiver is on channel i now
static bool dual_here(const radio_sm_t *sm, uint8_t i)
{
	const radio_dual_ref_t *r = &sm->dual[i];
	return r->vfo ? sm->view.vfo && sm->vfo_idx == r->pos :
			!sm->view.vfo && r->zone == sm->view.zone_index && r->pos == sm->view.zone_pos;
}

// A channel's zone (the current one, or read into buf) and its settings; NULL when it can't be watched. buf may be
// NULL without CONFIG_DUAL_WATCH: only the current zone then
static const zone_t *dual_read(const radio_sm_t *sm, const radio_dual_ref_t *r, zone_t *buf, channel_t *ch)
{
	const zone_t *z = NULL;
	if (r->vfo) {
		return NULL;
	}
	if (sm->have_channels && r->zone == sm->view.zone_index) {
		z = &sm->zone;
	}
#if CONFIG_DUAL_WATCH
	else if (read_zone(r->zone, buf)) {
		z = buf;
	}
#else
	(void)buf;
#endif
	return z != NULL && r->pos < z->count && codeplug_channel(z->channels[r->pos], ch) ? z : NULL;
}

static bool dual_ok(const radio_sm_t *sm, const radio_dual_ref_t *r)
{
	if (r->vfo) {
		return CONFIG_DUAL_WATCH && VFO_AVAILABLE;
	}
	channel_t ch;
#if CONFIG_DUAL_WATCH
	zone_t z;
	return dual_read(sm, r, &z, &ch) != NULL;
#else
	return dual_read(sm, r, NULL, &ch) != NULL;
#endif
}

#if CONFIG_DUAL_WATCH
// Channel i's changes for now, as they are while the receiver is on it
static void dual_keep_save(radio_sm_t *sm, uint8_t i)
{
	radio_dual_keep_t *k = &sm->dual_keep[i];
	const channel_t *ch = &sm->view.ch;
	k->valid = true;
	k->temp_power = sm->temp_power;
	k->power = sm->view.power;
	k->temp_squelch = sm->temp_squelch;
	k->squelch = sm->view.squelch;
	k->temp_ts = sm->temp_ts;
	k->bw = ch->bw;
	k->mode = ch->mode;
	k->color_code = ch->color_code;
	k->timeslot = ch->timeslot;
	k->rx_tone = ch->rx_tone;
	k->tx_tone = ch->tx_tone;
}

// ... and back on it (the channel or the VFO just loaded, not tuned yet)
static void dual_keep_load(radio_sm_t *sm, uint8_t i)
{
	const radio_dual_keep_t *k = &sm->dual_keep[i];
	if (!k->valid) {
		return;
	}
	if (k->temp_power) {
		sm->temp_power = true;
		sm->view.power = k->power;
	}
	if (k->temp_squelch) {
		sm->temp_squelch = true;
		sm->view.squelch = k->squelch;
	}
	if (!sm->view.vfo) {	// the VFO keeps its own settings
		channel_t *ch = &sm->view.ch;
		ch->bw = k->bw;
		ch->mode = k->mode;
		ch->color_code = k->color_code;
		ch->timeslot = k->timeslot;
		ch->rx_tone = k->rx_tone;
		ch->tx_tone = k->tx_tone;
		sm->temp_ts = k->temp_ts;
	}
	set_temporary(sm);
}
#endif

// Tune channel i (the VFO with its own settings), with its changes for now. False when it can't be read now (the
// codeplug changed)
static bool dual_tune(radio_sm_t *sm, uint8_t i, uint32_t now_ms)
{
	const radio_dual_ref_t *r = &sm->dual[i];
	channel_t ch;
#if CONFIG_DUAL_WATCH
	sm->dual_peek = false;	// a look at the other channel ends with any tune (dual_wait_service starts one after)
	if (sm->dual_at != i && dual_here(sm, sm->dual_at)) {
		dual_keep_save(sm, sm->dual_at);
	}
	if (r->vfo) {
		if (!VFO_AVAILABLE) {
			return false;
		}
		vfo_use(sm, (uint8_t)r->pos);	// each channel its own VFO
		enter_vfo(sm, now_ms);
		dual_keep_load(sm, i);
		sm->dual_at = i;
		return true;
	}
	zone_t buf;
	const zone_t *z = dual_read(sm, r, &buf, &ch);
#else
	const zone_t *z = dual_read(sm, r, NULL, &ch);
#endif
	if (z == NULL) {
		return false;
	}
	if (z != &sm->zone) {
		set_zone(sm, r->zone, z);	// the view reports the tuned channel's zone, as it does its position
	}
	set_channel(sm, r->pos, &ch);
#if CONFIG_DUAL_WATCH
	dual_keep_load(sm, i);
#endif
	tune(sm, now_ms);
	sm->dual_at = i;
	return true;
}

#if CONFIG_DUAL_WATCH
// Two receivers (board.rx2, hal_rx2.h): the main receiver stays on the selected channel and the second one gets the
// other, so dual watch hears both at once. While the second one is down or refuses the channel, the main receiver
// alternates as on any radio, and the second is tried again after RADIO_RX2_RETRY_MS

// Channel i's settings for the second receiver. False: it can't be read now
static bool rx2_settings(radio_sm_t *sm, uint8_t i, rx2_tune_t *t)
{
	static zone_t buf;		// only the radio task uses it
	const radio_dual_ref_t *r = &sm->dual[i];
	channel_t ch;
	memset(t, 0, sizeof(*t));
	if (r->vfo) {
		if (!VFO_AVAILABLE) {
			return false;
		}
		ch = r->pos == sm->vfo_idx ? sm->vfo_ch : sm->vfo_other.ch;
	} else if (dual_read(sm, r, &buf, &ch) == NULL) {
		return false;
	}
	t->rx_hz = ch.rx_hz;
	t->mode = ch.mode;
	t->bw = ch.bw;
	t->squelch = sm->view.squelch;
	t->rx_tone = ch.rx_tone;
	return true;
}

static void rx2_publish(radio_sm_t *sm)
{
	if (sm->view.rx2 != sm->rx2_on) {
		sm->view.rx2 = sm->rx2_on;
		changed(sm);
	}
}

static void rx2_release(radio_sm_t *sm)
{
	if (sm->rx2_on) {
		sm->rx2_on = false;
		board.rx2->stop();
		outputs(sm);
		rx2_publish(sm);
	}
}

// Give the second receiver the channel not selected, and the main receiver the selected one. False: it can't now
static bool rx2_engage(radio_sm_t *sm, uint32_t now_ms)
{
	rx2_tune_t t;
	sm->rx2_try_ms = sm->rx2_refresh_ms = now_ms;
	if (board.rx2 == NULL || !rx2_settings(sm, (uint8_t)!sm->dual_sel, &t) || !board.rx2->tune(&t)) {
		rx2_release(sm);
		return false;
	}
	if (!dual_here(sm, sm->dual_sel)) {
		if (!dual_tune(sm, sm->dual_sel, now_ms)) {
			rx2_release(sm);
			return false;
		}
		scan_tuned(&sm->scan, now_ms);
	}
	sm->dual_at = sm->dual_sel;
	sm->rx2_on = true;
	rx2_publish(sm);
	return true;
}

// Every poll in dual watch on a radio with a second receiver. True: it has the other channel (nothing to alternate)
static bool rx2_service(radio_sm_t *sm, uint32_t now_ms)
{
	rx2_status_t st;
	board.rx2->status(&st);
	if (!sm->rx2_on) {
		if (st.up && now_ms - sm->rx2_try_ms >= RADIO_RX2_RETRY_MS) {
			return rx2_engage(sm, now_ms);
		}
		return false;
	}
	if (!st.up || (!st.tuned && now_ms - sm->rx2_try_ms >= RADIO_RX2_TUNE_MS)) {
		rx2_release(sm);	// gone, or it won't take the channel: alternate until it's back
		sm->rx2_try_ms = now_ms;
		return false;
	}
	if (st.tuned) {
		sm->rx2_try_ms = now_ms;	// the deadline runs from the last time it was on its settings
	}
	if (now_ms - sm->rx2_refresh_ms >= RADIO_RX2_REFRESH_MS) {
		rx2_tune_t t;
		sm->rx2_refresh_ms = now_ms;
		if (rx2_settings(sm, (uint8_t)!sm->dual_sel, &t)) {
			board.rx2->tune(&t);	// a codeplug edit or a squelch change; the same settings again send nothing
		}
	}
	if (!dual_here(sm, sm->dual_sel) && dual_tune(sm, sm->dual_sel, now_ms)) {
		scan_tuned(&sm->scan, now_ms);
	}
	return true;
}
#else
static void rx2_release(radio_sm_t *sm) { (void)sm; }
#endif

static void scan_publish(radio_sm_t *sm)
{
	const scan_t *sc = &sm->scan;
	if (sm->view.scan != sc->kind || sm->view.scan_phase != sc->phase || sm->view.scan_skipped != sc->skipped) {
		sm->view.scan = sc->kind;
		sm->view.scan_phase = sc->phase;
		sm->view.scan_skipped = sc->skipped;
		sm->view.scan_probes = (uint16_t)sc->probes;	// with the phase changes, not a change of their own
		sm->view.scan_synced = (uint16_t)sc->stats_synced;
		changed(sm);
	}
}

// End the scan: stay where it is, or (cancel) go back to where it started
static void scan_stop(radio_sm_t *sm, bool cancel, uint32_t now_ms)
{
	scan_t *sc = &sm->scan;
	if (!scan_on(sc)) {
		return;
	}
	uint8_t kind = sc->kind;
	if (kind == SCAN_DUAL) {
		rx2_release(sm);
		dual_busy_set(sm, 0);
	}
	scan_end(sc);
	if (cancel) {
		if (kind == SCAN_DUAL) {
			if (!dual_here(sm, sm->dual_sel)) {
				dual_tune(sm, sm->dual_sel, now_ms);	// dual watch: back on the selected channel
			}
		} else if (kind == SCAN_VFO) {
			if (sm->view.ch.rx_hz != sc->origin_hz) {
				vfo_set_hz(sm, sc->origin_hz);
				tune(sm, now_ms);
			}
		} else if (sm->view.zone_pos != sc->origin_pos) {
			channel_t ch;
			if (codeplug_channel(sm->zone.channels[sc->origin_pos], &ch)) {
				set_channel(sm, sc->origin_pos, &ch);
				tune(sm, now_ms);
			}
		}
	}
	scan_publish(sm);
}

// Move to the next stop. False (and the scan ends) when there is none
static bool scan_step(radio_sm_t *sm, uint32_t now_ms)
{
	scan_t *sc = &sm->scan;
	if (sc->kind == SCAN_DUAL) {
		if (!dual_tune(sm, (uint8_t)!sm->dual_at, now_ms)) {
			scan_stop(sm, false, now_ms);
			return false;
		}
		dual_busy_set(sm, (uint8_t)(sm->view.dual_busy & ~(1u << sm->dual_at)));	// the watch hears it itself now
	} else if (sc->kind == SCAN_VFO) {
		uint32_t hz = scan_next_hz(sc, sm->view.ch.rx_hz, sm->view.vfo_step_hz);
		if (hz == 0 || !board_in_band(hz, false)) {
			scan_stop(sm, false, now_ms);
			return false;
		}
		vfo_set_hz(sm, hz);
		tune(sm, now_ms);
	} else {
		int pos = scan_next_pos(sc, sm->zone.count, scan_usable, sm);
		channel_t ch;
		if (pos < 0 || !codeplug_channel(sm->zone.channels[pos], &ch)) {
			scan_stop(sm, false, now_ms);
			return false;
		}
		set_channel(sm, (uint16_t)pos, &ch);
		tune(sm, now_ms);
	}
	scan_tuned(sc, now_ms);
	scan_publish(sm);
	return true;
}

static void scan_config(const radio_sm_t *sm, uint8_t kind, scan_cfg_t *cfg)
{
	memset(cfg, 0, sizeof(*cfg));
	cfg->dwell_ms = sm->set.scan_dwell_ms < SCAN_SETTLE_MS ? SCAN_SETTLE_MS : sm->set.scan_dwell_ms;
	cfg->hold_ms = (uint16_t)(sm->set.scan_hold_s * 1000u);
	cfg->delay_ms = sm->set.scan_delay_ms;
	cfg->resume = sm->set.scan_resume;
	cfg->priority = sm->set.scan_priority;
	if (kind == SCAN_DUAL && cfg->resume == SCAN_RESUME_STOP) {
		cfg->resume = SCAN_RESUME_CARRIER;	// a signal holds dual watch; it doesn't end it
	}
	if (kind == SCAN_DUAL) {
		cfg->delay_ms = sm->set.dual_delay_ms;
	}
}

static void scan_start(radio_sm_t *sm, uint8_t kind, uint32_t lo_hz, uint32_t hi_hz, uint32_t now_ms)
{
	scan_t *sc = &sm->scan;
	if (scan_on(sc)) {
		scan_stop(sm, false, now_ms);
	}
	if (kind == SCAN_VFO) {
		if (!sm->view.vfo) {
			return;
		}
		if (lo_hz == 0 && hi_hz == 0) {
			// The band the VFO is in
			for (uint8_t i = 0; i < board.band_count; i++) {
				if (sm->view.ch.rx_hz >= board.bands[i].lo_hz && sm->view.ch.rx_hz <= board.bands[i].hi_hz) {
					lo_hz = board.bands[i].lo_hz;
					hi_hz = board.bands[i].hi_hz;
				}
			}
		}
		// Both ends in one band, and room for two stops
		bool same_band = false;
		for (uint8_t i = 0; i < board.band_count; i++) {
			const band_t *b = &board.bands[i];
			if (lo_hz >= b->lo_hz && hi_hz <= b->hi_hz) {
				same_band = true;
			}
		}
		if (!same_band || hi_hz < lo_hz + sm->view.vfo_step_hz) {
			return;
		}
	} else if (sm->view.vfo || !sm->have_channels) {
		return;
	}
	scan_cfg_t cfg;
	scan_config(sm, kind, &cfg);
	sc->origin_vfo = sm->view.vfo;
	sc->origin_pos = sm->view.zone_pos;
	sc->origin_hz = sm->view.ch.rx_hz;
	sc->lo_hz = lo_hz;
	sc->hi_hz = hi_hz;
	scan_begin(sc, kind, &cfg, now_ms);
	if (kind != SCAN_VFO) {
		// Nothing to scan when no other channel qualifies
		scan_t probe = *sc;
		int next = scan_next_pos(&probe, sm->zone.count, scan_usable, sm);
		if (next < 0 || next == sm->view.zone_pos) {
			scan_end(sc);
			return;
		}
	}
	activity(sm, now_ms);
	scan_step(sm, now_ms);
}

// Dual watch on ref[0] and ref[1], ref[sel] selected. Asked again with the same pair: only the selection changes. A
// new pair keeps the receiver where it is (a call it holds too) if that channel is still one of the two, else it
// starts on the selected one
static void dual_start(radio_sm_t *sm, const radio_dual_ref_t ref[2], uint8_t sel, uint32_t now_ms)
{
	scan_t *sc = &sm->scan;
	if (sel > 1 || dual_same(&ref[0], &ref[1]) || !dual_ok(sm, &ref[0]) || !dual_ok(sm, &ref[1])) {
		return;
	}
	bool running = dual_running(sm);
	if (scan_on(sc) && !running) {
		scan_stop(sm, false, now_ms);
	}
#if CONFIG_DUAL_WATCH
	for (uint8_t i = 0; i < 2; i++) {
		if (!dual_same(&sm->dual[i], &ref[i])) {
			sm->dual_keep[i].valid = false;	// another channel: its changes for now went with the old one
			dual_busy_set(sm, (uint8_t)(sm->view.dual_busy & ~(1u << i)));
		}
	}
#endif
	sm->dual[0] = ref[0];
	sm->dual[1] = ref[1];
	sm->dual_sel = sel;
	sm->view.dual_sel = sel;
	bool here = false;
	for (uint8_t i = 0; i < 2; i++) {
		if (dual_here(sm, i)) {
			sm->dual_at = i;
			here = true;
		}
	}
	if (running && here) {
#if CONFIG_DUAL_WATCH
		if (board.rx2 != NULL) {
			rx2_engage(sm, now_ms);	// the selection may have changed: the receivers swap channels
			scan_publish(sm);
		}
#endif
		return;
	}
	if (!running) {
		scan_cfg_t cfg;
		scan_config(sm, SCAN_DUAL, &cfg);
		scan_begin(sc, SCAN_DUAL, &cfg, now_ms);
		activity(sm, now_ms);
	}
#if CONFIG_DUAL_WATCH
	if (board.rx2 != NULL && rx2_engage(sm, now_ms)) {
		scan_publish(sm);
		return;
	}
#endif
	if (!here) {
		if (!dual_tune(sm, sel, now_ms)) {
			scan_end(sc);
			scan_publish(sm);
			return;
		}
		scan_tuned(sc, now_ms);
	}
	scan_publish(sm);
}

#if CONFIG_DUAL_WATCH
// PTT in dual watch: on the selected channel (tuned first if the receiver is on the other), and the watch goes on
// after the transmission (scan_talked every poll while it lasts)
static void dual_talk(radio_sm_t *sm, uint32_t now_ms)
{
	sm->dual_peek = false;
	if (!dual_here(sm, sm->dual_sel) && !dual_tune(sm, sm->dual_sel, now_ms)) {
		scan_stop(sm, false, now_ms);
		return;
	}
	scan_talked(&sm->scan, now_ms);
	scan_publish(sm);
}

// A request for the selected channel (the monitor, a change for now, a talk target) while the receiver is on the
// other: it tunes the selected one first, as PTT does, and the watch goes on from there. Monitor held pauses it there
// (scan_service)
static void dual_to_selected(radio_sm_t *sm, uint32_t now_ms)
{
	sm->dual_peek = false;
	if (dual_here(sm, sm->dual_sel)) {
		return;
	}
	if (!dual_tune(sm, sm->dual_sel, now_ms)) {
		scan_stop(sm, false, now_ms);
		return;
	}
	scan_tuned(&sm->scan, now_ms);
	scan_publish(sm);
}

// Requests that act on the channel the receiver is on: in dual watch that must be the selected one
static bool for_selected(uint8_t type)
{
	switch (type) {
	case RREQ_POWER: case RREQ_SQUELCH: case RREQ_CLEAR_TEMP: case RREQ_DMR_TARGET: case RREQ_DMR_CONTACT:
	case RREQ_DMR_CALL_BACK: case RREQ_DMR_TARGET_CLEAR: case RREQ_DMR_HEAR_ALL:
		return true;
	default:
		return false;
	}
}

static radio_dual_ref_t dual_ref(uint32_t a)
{
	radio_dual_ref_t r = { false, 0, 0 };
	if ((a & RADIO_DUAL_VFO) != 0) {
		r.vfo = true;
		r.pos = (uint16_t)(a & 1u);
	} else {
		r.zone = (uint16_t)((a >> 16) & 0x3FFFu);
		r.pos = (uint16_t)a;
	}
	return r;
}

static void dual_request(radio_sm_t *sm, const radio_req_t *req, uint32_t now_ms)
{
	uint32_t a = (uint32_t)req->arg, b = req->arg2;
	if (sm->view.state == RS_TX || sm->hotspot || sm->ptt) {
		return;
	}
	if (((a | b) & RADIO_DUAL_SEL) == 0) {
		if (dual_running(sm)) {
			scan_stop(sm, true, now_ms);	// on the selected channel
		}
		sm->dual_keep[0].valid = sm->dual_keep[1].valid = false;	// the user stopped it: the other's go
		return;
	}
	radio_dual_ref_t ref[2] = { dual_ref(a), dual_ref(b) };
	dual_start(sm, ref, (b & RADIO_DUAL_SEL) != 0 ? 1 : 0, now_ms);
}
#endif

#if CONFIG_DUAL_WATCH
// Dual watch on one receiver, in the wait after a signal (SCAN_PH_RESUME): the receiver stays for the reply, but after
// each dwell there it looks at the other channel for one. A signal there is shown (view.dual_busy), not played or
// held: the receiver goes back and the wait goes on, and the watch goes there when the wait ends. A hold (a signal,
// the DMR reply window) has no looks. True while a look runs: this poll's readings are the other channel's
static bool dual_wait_service(radio_sm_t *sm, const radio_status_t *st, uint32_t now_ms)
{
	scan_t *sc = &sm->scan;
	uint32_t dwell = sc->cfg.dwell_ms;
	sc->cfg.delay_ms = sm->set.dual_delay_ms;	// a new setting applies at once
	if (sm->dual_peek) {
		uint32_t since = now_ms - sc->tuned_ms;
		if (since >= SCAN_SETTLE_MS && carrier_heard(sm, st->noise)) {
			sm->dual_peek_heard = true;
		}
		if (since < dwell) {
			return true;
		}
		uint8_t bit = (uint8_t)(1u << sm->dual_at);
		dual_busy_set(sm, sm->dual_peek_heard ? (uint8_t)(sm->view.dual_busy | bit) : (uint8_t)(sm->view.dual_busy & ~bit));
		if (!dual_tune(sm, (uint8_t)!sm->dual_at, now_ms)) {
			scan_stop(sm, false, now_ms);
			return true;
		}
		scan_tuned(sc, now_ms);
		sc->phase = SCAN_PH_RESUME;	// the wait goes on where it was
		sc->phase_ms = sm->dual_wait_ms;
		return true;
	}
	if (sc->phase != SCAN_PH_RESUME || sm->view.monitor || now_ms - sc->tuned_ms < dwell ||
	    now_ms - sc->phase_ms < dwell) {
		return false;
	}
	if (now_ms - sc->phase_ms + 2u * dwell > sc->cfg.delay_ms) {
		return false;	// no time for a look and a dwell back: the wait ends here
	}
	sm->dual_wait_ms = sc->phase_ms;
	uint8_t busy = sm->view.dual_busy;
	if (!dual_tune(sm, (uint8_t)!sm->dual_at, now_ms)) {
		scan_stop(sm, false, now_ms);
		return true;
	}
	scan_tuned(sc, now_ms);
	sc->phase = SCAN_PH_RESUME;
	sc->phase_ms = sm->dual_wait_ms;
	sm->dual_peek = true;
	sm->dual_peek_heard = false;
	sm->view.dual_busy = busy;	// the mark stays until this look says
	return true;
}
#endif


// Every poll while scanning: pause on the monitor, what the receiver hears, move on
static void scan_service(radio_sm_t *sm, const radio_status_t *st, uint32_t now_ms)
{
	scan_t *sc = &sm->scan;
#if CONFIG_DUAL_WATCH
	if (sc->kind == SCAN_DUAL && board.rx2 != NULL && rx2_service(sm, now_ms)) {
		scan_publish(sm);
		return;
	}
	if (sc->kind == SCAN_DUAL && dual_wait_service(sm, st, now_ms)) {
		scan_publish(sm);
		return;
	}
#endif
	scan_pause(sc, sm->view.monitor, now_ms);
	bool dmr = sm->view.ch.mode == RADIO_MODE_DMR;
	bool carrier = carrier_heard(sm, st->noise);
	bool active, sync = false;
#if RADIO_SM_DV
	if (sm->dv_running) {
		// P25: stop on frames with our NAC (or any, on a channel that hears every NAC); a call that plays holds the
		// stop. Another NAC's frames are a carrier only. The probe and sync windows are DMR's: a session starts,
		// then frames come every 180 ms at most
		radio_dv_status_t s;
		radio_dv_status(&s, now_ms);
		dmr = true;
		active = sm->view.state == RS_RX;
		sync = s.sync_ours;
		carrier = carrier || s.sync;
	} else
#endif
	{
		active = sm->view.state == RS_RX && (sm->view.squelch != 0 || carrier);
	}
	sc->cfg.probe_ms = dmr ? RADIO_SCAN_DMR_PROBE_MS : tone_needed(sm) ? RADIO_SCAN_FM_PROBE_MS : 0;
	sc->cfg.sync_ms = dmr ? RADIO_SCAN_DMR_SYNC_MS : 0;
	sc->cfg.carrier_hold_ms = dmr ? RADIO_SCAN_DMR_CARRIER_MS : 0;
	uint8_t was = sc->phase;
	switch (scan_poll(sc, now_ms, active, carrier, sync)) {
	case SCAN_ACT_NEXT:
		scan_step(sm, now_ms);
		return;
	case SCAN_ACT_STOP:
		scan_stop(sm, false, now_ms);
		break;
	default:
		break;
	}
	if (sc->phase == SCAN_PH_HOLD && was != SCAN_PH_HOLD && was != SCAN_PH_RESUME && sm->set.scan_beep) {
		beep(sm, BEEP_SCAN_HOLD);
	}
	scan_publish(sm);
}

// A scan stop asked for (the debug link): dual watch stops on the selected channel, as the UI's stop does, and the
// view counts it so the UI knows it was asked for and doesn't start the watch again
static void scan_stop_asked(radio_sm_t *sm, bool cancel, uint32_t now_ms)
{
	if (dual_running(sm)) {
		cancel = true;
		sm->view.dual_stops++;
#if CONFIG_DUAL_WATCH
		// The user's stop, as dual_request's: the UI asks for nothing after it, so the other's go here
		sm->dual_keep[0].valid = sm->dual_keep[1].valid = false;
#endif
	}
	scan_stop(sm, cancel, now_ms);
}

static void scan_request(radio_sm_t *sm, const radio_req_t *req, uint32_t now_ms)
{
	if (sm->view.state == RS_TX || sm->hotspot || sm->ptt) {
		return;
	}
	switch (req->arg) {
	case RSCAN_STOP:
		scan_stop_asked(sm, false, now_ms);
		break;
	case RSCAN_CANCEL:
		scan_stop_asked(sm, true, now_ms);
		break;
	case RSCAN_ZONE:
		scan_start(sm, SCAN_ZONE, 0, 0, now_ms);
		break;
	case RSCAN_VFO:
		scan_start(sm, SCAN_VFO, 0, 0, now_ms);
		break;
	case RSCAN_DUAL: {
		// The current channel (selected) and position arg2 of the current zone
		radio_dual_ref_t ref[2] = {
			{ sm->view.vfo, sm->view.vfo ? 0 : sm->view.zone_index, sm->view.vfo ? 0 : sm->view.zone_pos },
			{ false, sm->view.zone_index, (uint16_t)req->arg2 },
		};
		dual_start(sm, ref, 0, now_ms);
		break;
	}
	case RSCAN_TOGGLE:
		if (scan_on(&sm->scan)) {
			scan_stop_asked(sm, false, now_ms);
		} else {
			scan_start(sm, sm->view.vfo ? SCAN_VFO : SCAN_ZONE, 0, 0, now_ms);
		}
		break;
	case RSCAN_NUISANCE:
		if (scan_on(&sm->scan) && !dual_running(sm)) {	// dual watch: both channels were picked
			bool ok = sm->scan.kind == SCAN_VFO ? scan_nuisance_hz(&sm->scan, sm->view.ch.rx_hz)
				: scan_nuisance_pos(&sm->scan, sm->view.zone_pos);
			if (ok) {
				scan_step(sm, now_ms);
			}
		}
		break;
	default:
		break;
	}
}
#else
static void scan_stop(radio_sm_t *sm, bool cancel, uint32_t now_ms) { (void)sm; (void)cancel; (void)now_ms; }
static bool dual_running(const radio_sm_t *sm) { (void)sm; return false; }
static bool dual_peeking(const radio_sm_t *sm) { (void)sm; return false; }
#endif

static void persist(radio_sm_t *sm, uint32_t now_ms);

// --- DMR repeater roaming (radio/roam.c decides; this builds the site list, measures, tunes and keeps the block
// list; docs/spec/dmr-roaming.md) ---
static inline void roam_cancel(radio_sm_t *sm, uint32_t now_ms) { (void)sm; (void)now_ms; }
void radio_sm_shown_view(const radio_sm_t *sm, radio_view_t *v) { (void)sm; (void)v; }

#if CONFIG_APRS
// --- APRS transmissions (core/aprs, docs/spec/aprs.md 6) ---
// The engine (aprs_tick) decides what to send; here the radio leaves the user's channel for the APRS frequency (FM,
// no subtone), waits for a clear channel (p-persistent CSMA on the noise reading), keys up, lets board.afsk send the
// tones, and tunes back. Only from idle: no PTT, no reception, no scan, no hotspot, no DMR call or data, TX allowed.
// Any user action (PTT, a request) cuts it short and the engine tries again later.

#define APRS_SETTLE_MS		50	// the receiver settles on the APRS frequency before the first carrier check
#define APRS_SLOT_MS		100	// CSMA slot time
#define APRS_PERSIST		63	// p = (63 + 1) / 256 per clear slot
#define APRS_KEYUP_MS		25	// carrier up before the tones (the preamble flags follow)
#define APRS_SEND_MAX_MS	6000u	// longer than any tone buffer (4096 symbols = 3.4 s)

static bool aprs_can_start(const radio_sm_t *sm)
{
	if (sm->aprs_phase != APRS_RF_OFF || sm->ptt || sm->hotspot || (sm->view.state != RS_IDLE && !(sm->view.state == RS_RX && bcast_listening(sm))) ||
	    sm->view.monitor || sm->power_off_req || sm->set.tx_inhibit || sm->batt.level == BATT_CRITICAL || board.afsk == NULL ||
	    board.afsk->tx_start == NULL) {
		return false;
	}
#if CONFIG_SCAN
	if (scan_on(&sm->scan)) {
		return false;
	}
#endif
#if CONFIG_FM_TX_TONES
	if (sm->roger_ms != 0 || sm->burst_keyed) {
		return false;
	}
#endif
	return true;
}

// Back to the user's channel; how it ended for the engine
static void aprs_rf_end(radio_sm_t *sm, aprs_rf_result_t r, uint32_t now_ms)
{
	uint8_t phase = sm->aprs_phase;
	if (phase == APRS_RF_OFF) {
		return;
	}
	if (phase == APRS_RF_SEND) {
		board.afsk->tx_stop();
	}
	if (phase >= APRS_RF_KEYUP) {
		board.radio->tx_off();
		sm->tx_end_ms = now_ms;
	}
	sm->aprs_phase = APRS_RF_OFF;
	sm->view.state = RS_IDLE;
	aprs_rf_done(r, now_ms);
	tune(sm, now_ms);	// the channel again (DMR set up again if it was DMR)
}

static void aprs_rf_abort(radio_sm_t *sm, uint32_t now_ms)
{
	aprs_rf_end(sm, APRS_RF_ABORTED, now_ms);
}

static void aprs_rf_start(radio_sm_t *sm, uint32_t now_ms)
{
	static const subtone_t none = { TONE_NONE, 0 };
	if (!aprs_rf_take(&sm->aprs_job, now_ms)) {
		return;
	}
	if (sm->board_mode == RADIO_MODE_DMR) {
		dmr_end(sm);		// the call stack lets go of the modem before the chip leaves DMR
	}
	if (sm->board_mode != RADIO_MODE_FM) {
		sm->board_mode = RADIO_MODE_FM;
		board.radio->set_mode(RADIO_MODE_FM);
	}
	sm->aprs_phase = APRS_RF_LISTEN;
	sm->aprs_ms = now_ms;
	sm->aprs_listen_ms = now_ms;
	sm->aprs_rand ^= now_ms;
	board.radio->rx(sm->aprs_job.hz, sm->aprs_job.narrow ? RADIO_BW_12K5 : RADIO_BW_25K, &none);
	outputs(sm);
	changed(sm);
}

// Every poll while on the APRS frequency
static void aprs_rf_service(radio_sm_t *sm, const radio_status_t *st, uint32_t now_ms)
{
	static const subtone_t none = { TONE_NONE, 0 };
	switch (sm->aprs_phase) {
	case APRS_RF_LISTEN:
		if (now_ms - sm->aprs_listen_ms >= APRS_CSMA_GIVE_UP_MS) {
			aprs_rf_end(sm, APRS_RF_BUSY, now_ms);
			return;
		}
		if (now_ms - sm->aprs_ms < APRS_SETTLE_MS + APRS_SLOT_MS) {
			return;
		}
		sm->aprs_ms = now_ms - APRS_SETTLE_MS;	// the next check a slot later
		if (st->noise < radio_sm_squelch_open_below(RADIO_SQUELCH_DEFAULT)) {
			return;		// a carrier: wait
		}
		sm->aprs_rand = sm->aprs_rand * 1103515245u + 12345u;
		if ((sm->aprs_rand >> 24) > APRS_PERSIST) {
			return;		// clear, but not our turn this slot
		}
		sm->aprs_phase = APRS_RF_KEYUP;		// before tx(): aprs_rf_end unkeys from here on
		sm->aprs_ms = now_ms;
		sm->view.state = RS_TX;			// speaker off, red LED on, before keying
		sm->view.tx_auto = true;
		sm->view.last_tx_ms = now_ms != 0 ? now_ms : 1;
		sm->view.tx_deny = TXDENY_NONE;
		outputs(sm);
		changed(sm);
		// APRS's own power level when it has one, else the radio's current one (a temporary change included), as for
		// PTT. The settings' default (the board's default power) ignored a lowered level
		if (!board.radio->tx(sm->aprs_job.hz, sm->aprs_job.narrow ? RADIO_BW_12K5 : RADIO_BW_25K,
				     aprs_tx_power(sm->view.power), &none)) {
			aprs_rf_end(sm, APRS_RF_NO_KEY, now_ms);
		}
		break;
	case APRS_RF_KEYUP:
		if (now_ms - sm->aprs_ms < APRS_KEYUP_MS) {
			return;
		}
		if (!board.afsk->tx_start(sm->aprs_job.tones, sm->aprs_job.nbits, AFSK_BAUD, AFSK_MARK_HZ, AFSK_SPACE_HZ,
					  sm->aprs_job.level)) {
			aprs_rf_end(sm, APRS_RF_NO_AFSK, now_ms);
			return;
		}
		sm->aprs_phase = APRS_RF_SEND;
		sm->aprs_ms = now_ms;
		break;
	case APRS_RF_SEND:
		if (!board.afsk->tx_busy()) {
			aprs_rf_end(sm, APRS_RF_SENT, now_ms);
		} else if (now_ms - sm->aprs_ms > APRS_SEND_MAX_MS) {
			aprs_rf_end(sm, APRS_RF_TIMEOUT, now_ms);
		}
		break;
	default:
		break;
	}
}

// Every poll, before anything else: the engine, the "after PTT" trigger, a transmission to start
static void aprs_poll(radio_sm_t *sm, uint32_t now_ms)
{
	bool tx_now = sm->view.state == RS_TX && !sm->tx_no_ptt && sm->aprs_phase == APRS_RF_OFF;
	if (sm->aprs_was_tx && !tx_now) {
		aprs_ptt_end(now_ms);
	}
	sm->aprs_was_tx = tx_now;
	aprs_radio_t r;
	memset(&r, 0, sizeof(r));
	r.can_rf = aprs_can_start(sm);
	r.fm_rx = sm->aprs_phase == APRS_RF_OFF && sm->view.ch.mode == RADIO_MODE_FM && !sm->hotspot &&
		  sm->view.state != RS_TX;
	r.rx_hz = sm->view.ch.rx_hz;
	aprs_tick(&r, now_ms);
	if (r.can_rf && aprs_rf_pending()) {
		aprs_rf_start(sm, now_ms);
	}
}
#else
static inline void aprs_rf_abort(radio_sm_t *sm, uint32_t now_ms) { (void)sm; (void)now_ms; }
#endif

// --- DTMF (CONFIG_DTMF; radio-features.md 3.7): a digit key held during an FM transmission sends its pair ---

#if CONFIG_DTMF
// A digit key would send now: FM TX that PTT holds, with nothing else on the air
static bool dtmf_allowed(const radio_sm_t *sm)
{
	if (board.radio->tx_dtmf == NULL || sm->view.state != RS_TX || sm->view.ch.mode != RADIO_MODE_FM || !sm->ptt ||
			sm->hotspot || sm->host_audio) {
		return false;
	}
#if CONFIG_FM_TX_TONES
	if (sm->roger_ms != 0 || sm->view.tone_burst || sm->burst_keyed) {
		return false;
	}
#endif
	return true;
}

static void dtmf_start(radio_sm_t *sm, char digit, bool held, uint32_t now_ms)
{
	if (!board.radio->tx_dtmf(digit)) {
		return;
	}
	sm->view.dtmf = digit;
	sm->dtmf_held = held;
	sm->dtmf_on_ms = now_ms;
	sm->view.dtmf_sent++;
	size_t n = strlen(sm->view.dtmf_digits);
	if (n >= RADIO_DTMF_SHOWN) {
		memmove(sm->view.dtmf_digits, sm->view.dtmf_digits + 1, RADIO_DTMF_SHOWN - 1);	// keep the newest
		n = RADIO_DTMF_SHOWN - 1;
	}
	sm->view.dtmf_digits[n] = digit;
	sm->view.dtmf_digits[n + 1] = 0;
	changed(sm);
}

// The digit sounding off, the microphone back
static void dtmf_end(radio_sm_t *sm, uint32_t now_ms)
{
	board.radio->tx_dtmf(0);
	sm->view.dtmf = 0;
	sm->dtmf_held = false;
	sm->dtmf_off_ms = now_ms != 0 ? now_ms : 1;
	changed(sm);
}

// Everything off at once: the transmission ends, or something else takes the air
static void dtmf_cancel(radio_sm_t *sm, uint32_t now_ms)
{
	if (sm->view.dtmf != 0) {
		dtmf_end(sm, now_ms);
	}
	sm->dtmf_next = 0;
	if (sm->view.dtmf_ready) {
		sm->view.dtmf_ready = false;
		changed(sm);
	}
}

// Every poll while transmitting: the tone sounding ends once its key is up and it has had its shortest length; a
// waiting digit starts after the gap
static void dtmf_poll(radio_sm_t *sm, uint32_t now_ms)
{
	if (!dtmf_allowed(sm)) {
		dtmf_cancel(sm, now_ms);
		return;
	}
	if (!sm->view.dtmf_ready) {
		sm->view.dtmf_ready = true;
		changed(sm);
	}
	if (sm->view.dtmf != 0 && !sm->dtmf_held && now_ms - sm->dtmf_on_ms >= RADIO_DTMF_MIN_MS) {
		dtmf_end(sm, now_ms);
	}
	if (sm->view.dtmf == 0 && sm->dtmf_next != 0 &&
			(sm->dtmf_off_ms == 0 || now_ms - sm->dtmf_off_ms >= RADIO_DTMF_GAP_MS)) {
		char d = sm->dtmf_next;
		sm->dtmf_next = 0;
		dtmf_start(sm, d, sm->dtmf_next_held, now_ms);
	}
}

// RREQ_DTMF: a key down or up. Up always counts (a key held from before still lets go); down only while allowed
static void dtmf_key(radio_sm_t *sm, char digit, bool down, uint32_t now_ms)
{
	if (!down) {
		if (sm->view.dtmf == digit) {
			sm->dtmf_held = false;
		}
		if (sm->dtmf_next == digit) {
			sm->dtmf_next_held = false;
		}
		if (sm->view.state == RS_TX) {
			dtmf_poll(sm, now_ms);
		}
		return;
	}
	if (!dtmf_valid(digit) || !dtmf_allowed(sm)) {
		return;
	}
	activity(sm, now_ms);
	if (sm->view.dtmf == digit && sm->dtmf_held) {
		return;		// the same key again (a repeated edge)
	}
	if (sm->dtmf_next != 0) {
		return;		// one digit waits already: a third key in the rollover is dropped
	}
	// Another key down (rollover) ends the digit sounding once it has had its shortest length; this one waits for
	// that and the gap
	sm->dtmf_held = false;
	sm->dtmf_next = digit;
	sm->dtmf_next_held = true;
	dtmf_poll(sm, now_ms);
}
#else
static inline void dtmf_cancel(radio_sm_t *sm, uint32_t now_ms) { (void)sm; (void)now_ms; }
#endif

// --- FM transmit tones and the end of an FM transmission ---

// Back to receive after an FM transmission (PTT released, roger beep done, burst released)
// keyed: DMR per-burst keying is still armed (dmr_follow releases it), so no tx_off here
static void fm_tx_end(radio_sm_t *sm, bool keyed, uint32_t now_ms)
{
	dtmf_cancel(sm, now_ms);
#if CONFIG_DTMF
	sm->view.dtmf_digits[0] = 0;
#endif
	if (sm->view.state == RS_TX && !keyed) {
		board.radio->tx_off();	// ends a sub-audio tail too
	}
#if CONFIG_SUBTONE
	sm->tail_ms = 0;
#endif
	if (sm->view.state == RS_TX || sm->view.state == RS_TX_DENIED) {
		sm->view.state = RS_IDLE;
		sm->view.tx_deny = TXDENY_NONE;
		sm->view.tot_warning = false;
		sm->view.tone_burst = false;
		sm->view.roger = false;
		sm->close_count = 0;
		reset_tone(sm);
		outputs(sm);
		changed(sm);
	}
	sm->tx_end_ms = now_ms;
	if (sm->retune_pending) {
		sm->retune_pending = false;
		tune(sm, now_ms);	// a temporary bandwidth or RX tone was undone during TX
	}
}

#if CONFIG_SUBTONE
// The sub-audio tail at the end of an FM transmission (the DCS turn-off): the board starts it, the core holds the
// carrier RADIO_SUBTONE_TAIL_MS, then unkeys. True when it runs (the poll unkeys)
static bool tail_start(radio_sm_t *sm, uint32_t now_ms)
{
	if (sm->view.state != RS_TX || sm->view.ch.mode != RADIO_MODE_FM || board.radio->tx_tail == NULL ||
			!board.radio->tx_tail(true)) {
		return false;
	}
	sm->tail_ms = now_ms != 0 ? now_ms : 1;
	return true;
}

static void tail_cancel(radio_sm_t *sm)
{
	if (sm->tail_ms != 0) {
		board.radio->tx_tail(false);
		sm->tail_ms = 0;
	}
}

static void tail_poll(radio_sm_t *sm, uint32_t now_ms)
{
	if (now_ms - sm->tail_ms >= RADIO_SUBTONE_TAIL_MS) {
		tail_cancel(sm);
		fm_tx_end(sm, false, now_ms);
	}
}
#endif

// The end of an FM transmission the user let go of: the sub-audio tail first, if the board sends one
static void fm_unkey(radio_sm_t *sm, bool keyed, uint32_t now_ms)
{
#if CONFIG_SUBTONE
	if (!keyed && tail_start(sm, now_ms)) {
		return;		// tail_poll unkeys
	}
#endif
	fm_tx_end(sm, keyed, now_ms);
}

#if CONFIG_FM_TX_TONES
static bool tx_tone(uint16_t hz)
{
	return board.radio->tx_tone != NULL && board.radio->tx_tone(hz);
}

// The roger beep's second tone, then its end (every poll while it runs)
static void roger_poll(radio_sm_t *sm, uint32_t now_ms)
{
	uint32_t t = now_ms - sm->roger_ms;
	if (t >= 2u * RADIO_ROGER_STEP_MS) {
		tx_tone(0);
		sm->roger_ms = 0;
		sm->view.roger = false;
		fm_unkey(sm, false, now_ms);
	} else if (t >= RADIO_ROGER_STEP_MS && t < RADIO_ROGER_STEP_MS + RADIO_POLL_MS) {
		tx_tone(RADIO_ROGER_HZ2);
	}
}

static void roger_cancel(radio_sm_t *sm)
{
	if (sm->roger_ms != 0) {
		tx_tone(0);
		sm->roger_ms = 0;
		sm->view.roger = false;
		changed(sm);
	}
}
#endif

static void ptt_down(radio_sm_t *sm, uint32_t now_ms)
{
	if (sm->ptt) {
		return;
	}
	aprs_rf_abort(sm, now_ms);	// back on the channel first: PTT talks there
	activity(sm, now_ms);
	roam_cancel(sm, now_ms);	// PTT transmits on the site the user saw before a roaming search
#if CONFIG_FM_TX_TONES
	if (sm->roger_ms != 0 && sm->view.state == RS_TX) {
		roger_cancel(sm);	// pressed again during the roger beep: the transmission goes on
		sm->ptt = true;
		return;
	}
#endif
#if CONFIG_SUBTONE
	if (sm->tail_ms != 0 && sm->view.state == RS_TX) {
		tail_cancel(sm);	// pressed again during the tail: the transmission goes on
		sm->ptt = true;
		return;
	}
	sm->tail_ms = 0;	// a transmission that ended some other way (tx_off() ended its tail)
#endif
#if CONFIG_SCAN
#if CONFIG_DUAL_WATCH
	if (dual_running(sm)) {
		dual_talk(sm, now_ms);
	} else
#endif
	if (scan_on(&sm->scan)) {
		// On a signal: talk back on it. Still searching: back to the start channel
		bool on_signal = sm->scan.phase == SCAN_PH_HOLD || sm->scan.phase == SCAN_PH_RESUME;
		scan_stop(sm, !on_signal, now_ms);
	}
#endif
	sm->ptt = true;
	sm->tot_warned = false;
	sm->view.tot_warning = false;
	if (sm->hotspot) {
		deny(sm, TXDENY_HOTSPOT);	// the host link transmits for the hotspot; PTT does nothing until it ends
		return;
	}
	end_reception(sm, now_ms);

	uint8_t reason = tx_check(sm);
	if (reason != TXDENY_NONE) {
		deny(sm, reason);
		return;
	}
#if RADIO_SM_DV
	if (sm->dv_running) {
		dv_ptt_down(sm, now_ms);
		return;
	}
#endif
	sm->view.state = RS_TX;		// speaker off, red LED on, before keying
	sm->view.tx_auto = false;
	sm->view.last_tx_ms = now_ms != 0 ? now_ms : 1;
	sm->view.tx_deny = TXDENY_NONE;
	sm->view.tx_elapsed_ms = 0;
	sm->tx_start_ms = now_ms;
	outputs(sm);
	const channel_t *ch = &sm->view.ch;
	if (!board.radio->tx(ch->tx_hz, (radio_bw_t)ch->bw, sm->view.power, &ch->tx_tone)) {
		deny(sm, TXDENY_BAND);
		return;
	}
	changed(sm);
}

// roger: send the roger beep (if set) before unkeying
static void ptt_release(radio_sm_t *sm, bool roger, uint32_t now_ms)
{
	if (!sm->ptt) {
		return;
	}
	sm->ptt = false;
	activity(sm, now_ms);
#if RADIO_SM_DV
	if (sm->dv_permit_ms != 0) {
		sm->dv_permit_ms = 0;	// let go during the talk permit: never keyed
		return;
	}
	if (sm->dv_running && sm->view.state == RS_TX) {
		radio_dv_ptt(false);	// the terminator goes out; dv_follow ends RS_TX once it is off air
		dv_follow(sm, now_ms);
		return;
	}
#endif
	if (sm->host_audio) {
		roger = false;		// the host sends its own audio, tones included
	}
	bool keyed = false;
	dtmf_cancel(sm, now_ms);	// before the roger beep: the board never mixes the two
#if CONFIG_FM_TX_TONES
	if (sm->view.tone_burst) {
		tx_tone(0);
		sm->view.tone_burst = false;
	}
	sm->burst_keyed = false;
	if (roger && !keyed && sm->set.roger_beep && sm->view.state == RS_TX && sm->view.ch.mode == RADIO_MODE_FM &&
			tx_tone(RADIO_ROGER_HZ1)) {
		sm->roger_ms = now_ms != 0 ? now_ms : 1;
		sm->view.roger = true;
		changed(sm);
		return;		// roger_poll unkeys
	}
#else
	(void)roger;
#endif
	fm_unkey(sm, keyed, now_ms);
}

static void ptt_up(radio_sm_t *sm, uint32_t now_ms)
{
	ptt_release(sm, true, now_ms);
}

// --- Hotspot mode (core/hotspot drives the modem; hotspot_radio_ops_t in radio_task.c posts these) ---

bool radio_sm_hotspot_freq_ok(uint32_t rx_hz, uint32_t tx_hz)
{
	return rx_hz != 0 && tx_hz != 0 && board_in_band(rx_hz, false) && board_in_band(tx_hz, true);
}

uint8_t radio_sm_hotspot_power(uint8_t rf_level)
{
	if (board.power_count == 0) {
		return 0;
	}
	return (uint8_t)(((uint32_t)rf_level * (board.power_count - 1u) + 127u) / 255u);
}


// --- Settings: what the radio was set to, back at power-on (core/settings), and saved when it changes ---

// The preferences the radio core uses: base squelch, master power, VFO step. The own DMR ID is the codeplug's
static void apply_prefs(radio_sm_t *sm, const settings_t *st)
{
	master_power = st->master_power;
	sm->vol_step = radio_volume_step(st->volume);
	if (st->squelch <= RADIO_SQUELCH_MAX && sm->base_squelch != st->squelch) {
		sm->base_squelch = st->squelch;
		if (!sm->temp_squelch) {
			sm->view.squelch = st->squelch;
		}
	}
	if (!sm->temp_power) {
		sm->view.power = channel_power(&sm->view.ch);
	}
	sm->vfo_step_set = st->vfo_step_hz >= 1000u && st->vfo_step_hz <= 1000000u ? st->vfo_step_hz : 0;
	step_update(sm);
	set_temporary(sm);
}

// The VFO as saved, if its frequency still fits the board
static void vfo_defaults(channel_t *c)
{
	memset(c, 0, sizeof(*c));
	strcpy(c->name, "VFO");
	c->mode = VFO_MODE_DEFAULT;
	c->bw = RADIO_BW_12K5;
	c->power = CODEPLUG_POWER_DEFAULT;
	c->timeslot = 1;
	c->color_code = 1;	// the settings' default too: the usual repeater color code
	c->nac_rx = c->nac_tx = CODEPLUG_NAC_DEFAULT;
	c->talkgroup = 1;
}

// One saved VFO, whichever settings fields hold it: VFO 0 the first ones (packed), VFO 1 page 1's
typedef struct {
	uint32_t rx_hz, tx_hz;
	uint32_t talk;		// ID in bits 0-23, bit 31 private call
	uint16_t rx_tone, tx_tone;
	uint8_t mode, bw, power, cc, ts, rx_tone_type, tx_tone_type;
} vfo_saved_t;

static void vfo_saved_get(const settings_t *st, uint8_t idx, vfo_saved_t *o)
{
#if CONFIG_DUAL_WATCH
	if (idx != 0) {
		*o = (vfo_saved_t){ st->vfo2_rx_hz, st->vfo2_tx_hz, st->vfo2_talk, st->vfo2_rx_tone, st->vfo2_tx_tone,
				    st->vfo2_mode, st->vfo2_bw, st->vfo2_power, st->vfo2_cc, st->vfo2_ts,
				    st->vfo2_rx_tone_type, st->vfo2_tx_tone_type };
		return;
	}
#endif
	(void)idx;
	*o = (vfo_saved_t){ st->vfo_rx_hz, st->vfo_tx_hz,
			    settings_vfo_talk_id(st) | (settings_vfo_talk_private(st) ? 0x80000000u : 0),
			    st->vfo_rx_tone, st->vfo_tx_tone, settings_vfo_mode(st), settings_vfo_bw(st), st->vfo_power,
			    settings_vfo_cc(st), settings_vfo_ts(st), st->vfo_rx_tone_type, st->vfo_tx_tone_type };
}

static void restore_vfo(radio_sm_t *sm, const settings_t *st, uint8_t idx)
{
	vfo_saved_t v;
	vfo_saved_get(st, idx, &v);
	if (v.rx_hz == 0 || !board_in_band(v.rx_hz, false)) {
		return;
	}
	sm->vfo_ch.rx_hz = v.rx_hz;
	sm->vfo_ch.tx_hz = v.tx_hz != 0 ? v.tx_hz : v.rx_hz;
	// The saved mode if this build has that VFO; an older record holds FM
	sm->vfo_ch.mode = radio_vfo_mode_built(v.mode) ? v.mode : VFO_MODE_DEFAULT;
	if (codeplug_mode_digital_voice(sm->vfo_ch.mode)) {
		uint16_t max = sm->vfo_ch.mode == RADIO_MODE_M17 ? 15 : 0xFFF;
		sm->vfo_ch.nac_tx = v.tx_tone <= max && (sm->vfo_ch.mode == RADIO_MODE_M17 || v.tx_tone < CODEPLUG_NAC_ANY) ? v.tx_tone : (sm->vfo_ch.mode == RADIO_MODE_M17 ? 0 : CODEPLUG_NAC_DEFAULT);
		sm->vfo_ch.nac_rx = v.rx_tone <= max || (sm->vfo_ch.mode==RADIO_MODE_M17&&v.rx_tone==0xffff) ? v.rx_tone : sm->vfo_ch.nac_tx;
		sm->vfo_ch.talkgroup = (v.talk & 0xFFFFFFu) > 0 && (v.talk & 0xFFFFFFu) <= 65535 ? (uint16_t)v.talk : 1;
	}
	sm->vfo_ch.bw = v.bw == RADIO_BW_25K ? RADIO_BW_25K : RADIO_BW_12K5;
	sm->vfo_ch.power = v.power < board.power_count ? v.power : CODEPLUG_POWER_DEFAULT;
	sm->vfo_ch.color_code = v.cc <= 15 ? v.cc : 1;
	sm->vfo_ch.timeslot = v.ts == 2 ? 2 : 1;
	subtone_t rx = { v.rx_tone_type, v.rx_tone }, tx = { v.tx_tone_type, v.tx_tone };
	sm->vfo_ch.rx_tone = tones_valid(&rx) ? rx : (subtone_t){ TONE_NONE, 0 };
	sm->vfo_ch.tx_tone = tones_valid(&tx) ? tx : (subtone_t){ TONE_NONE, 0 };
	sm->vfo_seeded = true;
}

// The VFO in use (vfo_ch and its target) into VFO idx's settings fields
static void persist_vfo(const radio_sm_t *sm, settings_t *st, uint8_t idx)
{
	const channel_t *v = &sm->vfo_ch;
	uint8_t power = v->power == CODEPLUG_POWER_DEFAULT ? SETTINGS_UNSET8 : v->power;
	// Digital modes have no analog tones: reuse their saved values for NAC/RAN, without changing the settings record.
	bool dv = codeplug_mode_digital_voice(v->mode);
	subtone_t rx = dv ? (subtone_t){ TONE_NONE, v->nac_rx } : v->rx_tone;
	subtone_t tx = dv ? (subtone_t){ TONE_NONE, v->nac_tx } : v->tx_tone;
#if CONFIG_M17
	uint32_t talk = dv ? v->talkgroup : sm->vfo_talk_id | (sm->vfo_talk_private ? 0x80000000u : 0);
#endif
#if CONFIG_DUAL_WATCH
	if (idx != 0) {
		if (sm->vfo_seeded) {
			st->vfo2_rx_hz = v->rx_hz;
			st->vfo2_tx_hz = v->tx_hz;
			st->vfo2_mode = v->mode;
			st->vfo2_bw = v->bw;
			st->vfo2_power = power;
			st->vfo2_cc = v->color_code;
			st->vfo2_ts = v->timeslot;
			st->vfo2_rx_tone_type = rx.type;
			st->vfo2_rx_tone = rx.value;
			st->vfo2_tx_tone_type = tx.type;
			st->vfo2_tx_tone = tx.value;
		}
#if CONFIG_M17
		st->vfo2_talk = talk;
#endif
		return;
	}
#endif
	(void)idx;
	if (sm->vfo_seeded) {
		st->vfo_rx_hz = v->rx_hz;
		st->vfo_tx_hz = v->tx_hz;
		settings_set_vfo_mode_bw(st, v->mode, v->bw);
		st->vfo_power = power;
		settings_set_vfo_cc_ts(st, v->color_code, v->timeslot);
		st->vfo_rx_tone_type = rx.type;
		st->vfo_rx_tone = rx.value;
		st->vfo_tx_tone_type = tx.type;
		st->vfo_tx_tone = tx.value;
	}
#if CONFIG_M17
	settings_set_vfo_talk(st, talk & 0xFFFFFFu, (talk & 0x80000000u) != 0);
#endif
}

// The position of channel number in z, or -1
static int zone_find(const zone_t *z, uint16_t number)
{
	for (uint16_t i = 0; number != 0 && i < z->count; i++) {
		if (z->channels[i] == number) {
			return i;
		}
	}
	return -1;
}

// Zone zi, on channel number (or else near pos); false if the zone has no readable channel
static bool select_in_zone(radio_sm_t *sm, uint16_t zi, uint16_t number, uint16_t pos)
{
	zone_t z;
	uint16_t p;
	channel_t ch;
	if (!read_zone(zi, &z)) {
		return false;
	}
	int at = zone_find(&z, number);
	if (at >= 0 && codeplug_channel(number, &ch)) {
		p = (uint16_t)at;
	} else if (!find_channel(&z, pos < z.count ? pos : 0, 1, &p, &ch)) {
		return false;
	}
	set_zone(sm, zi, &z);
	set_channel(sm, p, &ch);
	sm->have_channels = true;
	return true;
}

// Zone and channel: the saved ones, else zone 0's first channel, else the first zone that has one
static void select_position(radio_sm_t *sm, uint16_t zi, uint16_t number, uint16_t pos)
{
	uint16_t zones = codeplug_zone_count();
	sm->have_channels = false;
	if (zi < zones && select_in_zone(sm, zi, number, pos)) {
		return;
	}
	for (uint16_t i = 0; i < zones; i++) {
		if (select_in_zone(sm, i, 0, 0)) {
			return;
		}
	}
	memset(&sm->zone, 0, sizeof(sm->zone));
}

// Note the position and the VFO in the settings (saved later, when they rest; settings_end only marks changes)
static void persist(radio_sm_t *sm, uint32_t now_ms)
{
	if (sm->hotspot) {
		return;		// the hotspot's frequencies are the host's, not the radio's
	}
	settings_t *st = settings_begin();
	bool vfo = sm->view.vfo;
	uint8_t vfo_index = sm->vfo_idx;
	uint16_t zone = sm->view.zone_index, pos = sm->view.zone_pos;
	const zone_t *z = sm->have_channels ? &sm->zone : NULL;
#if CONFIG_SCAN && CONFIG_DUAL_WATCH
	zone_t buf;
	if (dual_running(sm) && !dual_here(sm, sm->dual_sel)) {
		// Dual watch on the other channel now: power up on the selected one
		const radio_dual_ref_t *r = &sm->dual[sm->dual_sel];
		vfo = r->vfo;
		if (r->vfo) {
			vfo_index = (uint8_t)r->pos;	// power up in the selected channel's VFO
		} else {
			if (r->zone != zone) {
				z = read_zone(r->zone, &buf) ? &buf : NULL;
			}
			zone = r->zone;
			pos = r->pos;
		}
	}
#endif
	st->vfo = vfo;
	// Zones moved and not followed yet: the indexes here are the old ones, and the zone write saved the new ones
	// (zone_settle.c). Checked under the settings lock, so a write can't come between
	if (z != NULL && !ZONES_MOVED(sm)) {
		st->zone_index = zone;
		st->zone_pos = pos;
		st->channel = pos < z->count ? z->channels[pos] : 0;
	}
	persist_vfo(sm, st, sm->vfo_idx);
#if CONFIG_DUAL_WATCH
	uint8_t in_use = sm->vfo_idx;
	vfo_use(sm, (uint8_t)!in_use);		// the other VFO into its own fields: swap, write, swap back
	persist_vfo(sm, st, sm->vfo_idx);
	vfo_use(sm, in_use);
	st->vfo_index = vfo_index;
#else
	(void)vfo_index;
#endif
	settings_end(now_ms);
}

static void clamp_settings(radio_settings_t *s)
{
	if (s->beep_level > 100) {
		s->beep_level = 100;
	}
	if (s->vox_level > 10) {
		s->vox_level = 10;
	}
	if (s->rx_save > 3) {
		s->rx_save = 3;
	}
	if (s->reply_s > 30 && s->reply_s != RADIO_REPLY_CODEPLUG) {
		s->reply_s = 30;
	}
	if (s->dual_delay_ms > RADIO_DUAL_DELAY_MAX_MS) {
		s->dual_delay_ms = RADIO_DUAL_DELAY_MAX_MS;
	}
	if (s->bcast_step > RADIO_BCAST_STEP_100) {
		s->bcast_step = RADIO_BCAST_STEP_200_ODD;
	}
	if (s->roam_rssi_dbm < -125) {
		s->roam_rssi_dbm = -125;
	} else if (s->roam_rssi_dbm > -60) {
		s->roam_rssi_dbm = -60;
	}
	if (s->roam_hyst_db < 1) {
		s->roam_hyst_db = 1;
	} else if (s->roam_hyst_db > 30) {
		s->roam_hyst_db = 30;
	}
	if (s->roam_probe > 1) {
		s->roam_probe = 1;
	}
	if (s->roam_check_s != 0 && s->roam_check_s < 30) {
		s->roam_check_s = 30;
	} else if (s->roam_check_s > 3600) {
		s->roam_check_s = 3600;
	}
	if (s->roam_beacon_s != 0 && s->roam_beacon_s < 10) {
		s->roam_beacon_s = 10;
	} else if (s->roam_beacon_s > 600) {
		s->roam_beacon_s = 600;
	}
	if (s->roam_beacon_len_s < 1) {
		s->roam_beacon_len_s = 1;
	} else if (s->roam_beacon_len_s > 10) {
		s->roam_beacon_len_s = 10;
	}
	if (s->roam_survey > 2) {
		s->roam_survey = 1;
	}
	// 0 repeat only, 1 both (first arrival), 2 both (dual-path RX, CONFIG_SFR_DUALPATH_RX; else first arrival)
	if (s->sfr_rx > 2) {
		s->sfr_rx = 0;
	} else if (s->sfr_rx == 2 && !CONFIG_SFR_DUALPATH_RX) {
		s->sfr_rx = 1;
	}
	if (s->sfr_cwid > 4 || !CONFIG_SFR_CWID) {
		s->sfr_cwid = 0;
	}
}

// The radio core's settings as kept in the settings store (docs/spec/settings-store.md 3); the defaults if never set
static void settings_to_radio(const settings_t *st, radio_settings_t *s)
{
	radio_settings_default(s);
	if (st->roam_stored == 1) {
		s->roam_rssi_dbm = st->roam_rssi_dbm;
		s->roam_hyst_db = st->roam_hyst_db;
		s->roam_probe = st->roam_probe;
		s->roam_check_s = st->roam_check_s;
		s->roam_beacon_s = st->roam_beacon_s;
		s->roam_beacon_len_s = st->roam_beacon_len_s;
		s->roam_survey = st->roam_survey;
		s->rpt_beacon = (st->rpt_beacon & SETTINGS_RPT_BEACON) != 0;
		s->sfr_rx = (uint8_t)((st->rpt_beacon & SETTINGS_RPT_SFR_RX) >> SETTINGS_RPT_SFR_RX_SHIFT);
		s->sfr_cwid = (uint8_t)((st->rpt_beacon & SETTINGS_RPT_SFR_CWID) >> SETTINGS_RPT_SFR_CWID_SHIFT);
		clamp_settings(s);
	}
	// Page 1: 0 (never set, or a page 0 alone) keeps the default
	if (st->rs_dual_delay != 0) {
		s->dual_delay_ms = (uint16_t)((st->rs_dual_delay - 1u) * 100u);
		clamp_settings(s);
	}
	if (st->rs_bcast_step != 0) {
		s->bcast_step = (uint8_t)(st->rs_bcast_step - 1u);
		clamp_settings(s);
	}
	if (st->rs_stored != 1) {
		return;
	}
	s->tot_s = st->rs_tot_s;
	s->tot_warn_s = st->rs_tot_warn_s;
	s->bcl = st->rs_bcl;
	s->roger_beep = (st->rs_flags & SETTINGS_RS_ROGER_BEEP) != 0;
	s->tx_inhibit = (st->rs_flags & SETTINGS_RS_TX_INHIBIT) != 0;
	s->scan_priority = (st->rs_flags & SETTINGS_RS_SCAN_PRIORITY) != 0;
	s->scan_beep = (st->rs_flags & SETTINGS_RS_SCAN_BEEP) != 0;
	s->key_beep = (st->rs_flags & SETTINGS_RS_KEY_BEEP) != 0;
	s->talk_permit = (st->rs_flags & SETTINGS_RS_NO_TALK_PERMIT) == 0;
	s->scan_resume = st->rs_scan_resume;
	s->scan_hold_s = st->rs_scan_hold_s;
	s->scan_delay_ms = st->rs_scan_delay_ms;
	s->scan_dwell_ms = st->rs_scan_dwell_ms;
	s->beep_level = st->rs_beep_level;
	s->vox = st->rs_vox;
	s->vox_level = st->rs_vox_level;
	s->vox_tail_ms = st->rs_vox_tail_ms;
	s->rx_save = st->rs_rx_save;
	s->rx_save_idle_s = st->rs_rx_save_idle_s;
	s->battery_off_s = st->rs_battery_off_s;
	s->backlight_dim_s = st->rs_backlight_dim_s;
	s->backlight_off_s = st->rs_backlight_off_s;
	s->reply_s = st->rs_reply_s;
	clamp_settings(s);
}

static void radio_to_settings(const radio_settings_t *s, uint32_t now_ms)
{
	settings_t *st = settings_begin();
	st->rs_tot_s = s->tot_s;
	st->rs_tot_warn_s = s->tot_warn_s;
	st->rs_bcl = s->bcl;
	st->rs_flags = (uint8_t)((s->roger_beep ? SETTINGS_RS_ROGER_BEEP : 0) | (s->tx_inhibit ? SETTINGS_RS_TX_INHIBIT : 0) |
				 (s->scan_priority ? SETTINGS_RS_SCAN_PRIORITY : 0) |
				 (s->scan_beep ? SETTINGS_RS_SCAN_BEEP : 0) | (s->key_beep ? SETTINGS_RS_KEY_BEEP : 0) |
				 (s->talk_permit ? 0 : SETTINGS_RS_NO_TALK_PERMIT));
	st->rs_scan_resume = s->scan_resume;
	st->rs_scan_hold_s = s->scan_hold_s;
	st->rs_scan_delay_ms = s->scan_delay_ms;
	st->rs_scan_dwell_ms = s->scan_dwell_ms;
	st->rs_beep_level = s->beep_level;
	st->rs_vox = s->vox;
	st->rs_vox_level = s->vox_level;
	st->rs_vox_tail_ms = s->vox_tail_ms;
	st->rs_rx_save = s->rx_save;
	st->rs_rx_save_idle_s = s->rx_save_idle_s;
	st->rs_battery_off_s = s->battery_off_s;
	st->rs_backlight_dim_s = s->backlight_dim_s;
	st->rs_backlight_off_s = s->backlight_off_s;
	st->rs_reply_s = s->reply_s;
	st->rs_dual_delay = (uint8_t)(s->dual_delay_ms / 100u + 1u);
	st->rs_bcast_step = (uint8_t)(s->bcast_step + 1u);
	st->rs_stored = 1;
	st->roam_rssi_dbm = s->roam_rssi_dbm;
	st->roam_hyst_db = s->roam_hyst_db;
	st->roam_probe = s->roam_probe;
	st->roam_check_s = s->roam_check_s;
	st->roam_beacon_s = s->roam_beacon_s;
	st->roam_beacon_len_s = s->roam_beacon_len_s;
	st->roam_survey = s->roam_survey;
	st->rpt_beacon = (uint8_t)((s->rpt_beacon ? SETTINGS_RPT_BEACON : 0) |
				   ((s->sfr_rx << SETTINGS_RPT_SFR_RX_SHIFT) & SETTINGS_RPT_SFR_RX) |
				   ((s->sfr_cwid << SETTINGS_RPT_SFR_CWID_SHIFT) & SETTINGS_RPT_SFR_CWID));
	st->roam_stored = 1;
	settings_end(now_ms);
}

static void use_settings(radio_sm_t *sm, const radio_settings_t *s, uint32_t now_ms)
{
	sm->set = *s;
	clamp_settings(&sm->set);
	apply_tot(sm);
	activity(sm, now_ms);
	changed(sm);
}

// From radio_set_settings (the settings screen, the debug link): in effect at once, saved by the settings store
void radio_sm_set_settings(radio_sm_t *sm, const radio_settings_t *s, uint32_t now_ms)
{
	use_settings(sm, s, now_ms);
	radio_to_settings(&sm->set, now_ms);
}

// The codeplug changed underneath (an edit, a CPS write) or the preferences did: read them again, keep the zone and
// the channel if they still exist
#if CONFIG_ZONE_WRITE
// A zone added ahead of others or a zone deleted moves the indexes after it (zone_edit.h). Followed here: the zone in
// use (in the VFO too: the zone it goes back to) and dual watch's two channels. The saved roaming zones were moved by
// the zone write itself (zone_settle.c; a deleted one loses its flag) and the reload reads them. Returns the zone in
// use's index now; *gone: it was deleted, and the index is the zone that took its place
static uint16_t follow_zones(radio_sm_t *sm, bool *gone, uint32_t now_ms)
{
	uint32_t from = sm->zone_shifts, to = zone_edit_shifts();
	uint16_t zi = sm->view.zone_index, out;
	sm->zone_shifts = to;
	*gone = false;
	if (from == to) {
		return zi;
	}
	zone_map_t m = zone_edit_map(from, to, zi, &out);
	if (m != ZONE_MAP_LOST) {
		zi = out;
		*gone = m == ZONE_MAP_GONE;
	}
#if CONFIG_SCAN
	for (uint8_t i = 0; i < 2; i++) {
		if (!sm->dual[i].vfo && zone_edit_map(from, to, sm->dual[i].zone, &out) != ZONE_MAP_LOST) {
			sm->dual[i].zone = out;		// a deleted one: the UI finds its channel again or ends the watch
		}
	}
#endif
	(void)now_ms;
	return zi;
}

#if CONFIG_SCAN
// A checksum of the zone in use and its channel list: an edit of it ends a zone scan (its skips are by position)
static uint32_t zone_sum(const radio_sm_t *sm)
{
	uint32_t h = sm->have_channels ? 2166136261u ^ sm->zone.count : 0;
	for (uint16_t i = 0; sm->have_channels && i < sm->zone.count; i++) {
		h = (h ^ sm->zone.channels[i]) * 16777619u;
	}
	return h ^ sm->view.zone_index;
}
#endif
#endif

static void reload(radio_sm_t *sm, uint32_t now_ms)
{
	settings_t *st = &st_copy;
	roam_cancel(sm, now_ms);	// back to the site the search started from first
	settings_read(st);
	sm->reload_pending = false;
	sm->codeplug_serial = codeplug_serial();
	sm->view.source = codeplug_source();
	uint16_t number = sm->have_channels && sm->view.zone_pos < sm->zone.count ? sm->zone.channels[sm->view.zone_pos] : 0;
	bool vfo = sm->view.vfo;
	uint16_t zi = sm->view.zone_index, pos = sm->view.zone_pos;
#if CONFIG_ZONE_WRITE && CONFIG_SCAN
	uint32_t sum = zone_sum(sm);	// read by the zone scan check below
#endif
#if CONFIG_ZONE_WRITE
	bool gone;
	zi = follow_zones(sm, &gone, now_ms);
	if (gone) {
		// The zone in use was deleted: the zone that took its place (the last one when it was the last), on the
		// same channel when that zone holds it, else its first channel
		uint16_t n = codeplug_zone_count();
		zi = n > 0 && zi >= n ? (uint16_t)(n - 1u) : zi;
		pos = 0;
	}
	sm->view.zone_index = zi;
#endif
	select_position(sm, zi, number, pos);
#if CONFIG_ZONE_WRITE && CONFIG_SCAN
	if (sm->scan.kind == SCAN_ZONE && zone_sum(sm) != sum) {
		scan_stop(sm, false, now_ms);
	}
#endif
	apply_prefs(sm, st);
	radio_settings_t rs;
	settings_to_radio(st, &rs);
	use_settings(sm, &rs, now_ms);
	if (vfo || !sm->have_channels) {
		if (VFO_AVAILABLE) {
			enter_vfo(sm, now_ms);
		} else {
			strcpy(sm->view.ch.name, "No channels");
			changed(sm);
		}
	} else {
		tune(sm, now_ms);
	}
}

#if CONFIG_BEEP
// The PCM stream the radio's own users (beeps, DMR voice) take: the board's, shared with a host audio session
static const hal_audio_t *pcm_audio(void)
{
#if CONFIG_APRS_RX
	return audio_mux_wrap(board.audio);
#else
	return board.audio;
#endif
}
#endif

void radio_sm_init(radio_sm_t *sm, uint32_t now_ms)
{
	settings_t st;
	settings_read(&st);	// a zone add or delete a power cut interrupted is settled first (radio_core_start)

	memset(sm, 0, sizeof(*sm));
	sm->audio_out = -1;
	sm->green_out = -1;
	sm->red_out = -1;
#if CONFIG_DUAL_WATCH
	sm->rx2_audio_out = -1;
#endif
	sm->volume_sent = -1;
	sm->vol_step = RADIO_VOLUME_DEFAULT;
	sm->vol_override = -1;
	sm->view.volume = -1;
	sm->view.volume_override = -1;
	sm->view.volume_step = RADIO_VOLUME_DEFAULT;
	sm->base_squelch = RADIO_SQUELCH_DEFAULT;
	sm->view.squelch = RADIO_SQUELCH_DEFAULT;
	settings_to_radio(&st, &sm->set);	// as saved (docs/spec/settings-store.md 3)
	sm->view.tot_s = sm->set.tot_s;
	sm->activity_ms = now_ms;
	batt_init(&sm->batt);
#if CONFIG_BEEP
	beep_init(pcm_audio());
#endif
	step_update(sm);
	sm->view.source = codeplug_source();
	sm->codeplug_serial = codeplug_serial();
#if CONFIG_ZONE_WRITE
	sm->zone_shifts = zone_edit_shifts();
#endif

#if CONFIG_DUAL_WATCH
	vfo_defaults(&sm->vfo_ch);	// the second VFO first, then moved to vfo_other
	restore_vfo(sm, &st, 1);
	sm->vfo_other = (radio_vfo_t){ sm->vfo_ch, sm->vfo_seeded, sm->vfo_talk_id, sm->vfo_talk_private };
	sm->vfo_seeded = false;
	sm->vfo_talk_id = 0;
	sm->vfo_talk_private = false;
#endif
	vfo_defaults(&sm->vfo_ch);
	restore_vfo(sm, &st, 0);
#if CONFIG_DUAL_WATCH
	vfo_use(sm, st.vfo_index);
#endif

	sm->board_mode = RADIO_MODE_FM;
	board.radio->set_mode(RADIO_MODE_FM);


	// The saved zone and channel; else zone 0 and its first channel; if zone 0 has none, the first zone that has one
	select_position(sm, st.zone_index, st.channel, st.zone_pos);
	apply_prefs(sm, &st);
	if (!sm->have_channels || (st.vfo && VFO_AVAILABLE)) {
		if (VFO_AVAILABLE) {
			enter_vfo(sm, now_ms);
		} else {
			strcpy(sm->view.ch.name, "No channels");	// built without a VFO and nothing programmed: nothing to tune
		}
	} else {
		tune(sm, now_ms);
	}
	sm->rssi_published = sm->view.rssi_dbm;
#if CONFIG_APRS
	aprs_init(now_ms);
#endif
}


#if CONFIG_FM_TX_TONES
// The 1750 Hz burst: on keys up (FM channel, the usual TX checks) if not transmitting yet
static void tone_burst(radio_sm_t *sm, bool on, uint32_t max_ms, uint32_t now_ms)
{
	if (on) {
		if (sm->view.tone_burst || sm->view.ch.mode != RADIO_MODE_FM || sm->hotspot ||
				board.radio->tx_tone == NULL) {
			return;
		}
		if (sm->view.state != RS_TX) {
			ptt_down(sm, now_ms);
			sm->burst_keyed = sm->ptt;	// held by the burst, even when refused (the deny shows until release)
			if (sm->view.state != RS_TX) {
				return;
			}
		}
		roger_cancel(sm);
		dtmf_cancel(sm, now_ms);
		if (tx_tone(RADIO_BURST_HZ)) {
			sm->view.tone_burst = true;
			sm->burst_end_ms = max_ms != 0 ? (now_ms + max_ms) | 1u : 0;	// | 1: never 0, "no limit"
			changed(sm);
		}
		return;
	}
	sm->burst_end_ms = 0;
	if (sm->view.tone_burst) {
		tx_tone(0);
		sm->view.tone_burst = false;
		changed(sm);
	}
	if (sm->burst_keyed) {
		sm->burst_keyed = false;
		ptt_release(sm, false, now_ms);
	}
}
#endif

// A request that changes what the receiver is tuned to ends a scan first (it stays where it is)
static bool retunes(uint8_t type)
{
	switch (type) {
	case RREQ_CH_STEP: case RREQ_ZONE_POS: case RREQ_ZONE: case RREQ_VFO: case RREQ_VFO_STEP: case RREQ_VFO_SET:
	case RREQ_VFO_TX: case RREQ_BANDWIDTH: case RREQ_TONE: case RREQ_MODE: case RREQ_DMR_TS: case RREQ_DMR_CC: case RREQ_VFO_COPY:
		return true;
	default:
		return false;
	}
}

void radio_sm_request(radio_sm_t *sm, const radio_req_t *req, uint32_t now_ms)
{
	// A beep or a vibration is an alert, not the user doing something; the volume changes nothing on air or on the
	// channel, so it neither gives way to nor cancels anything either
	bool alert = req->type == RREQ_BEEP || req->type == RREQ_VIBRATE || req->type == RREQ_VOLUME ||
		     req->type == RREQ_VOLUME_OVERRIDE;
	if (!alert && req->type != RREQ_DMR_FOCUS && req->type != RREQ_SETTINGS && req->type != RREQ_RETUNE) {
		aprs_rf_abort(sm, now_ms);	// the user does something: an APRS transmission gives way
	}
	bool busy = sm->view.state == RS_TX || sm->hotspot;	// no retuning while transmitting or a hotspot
	int dir = req->arg > 0 ? 1 : -1;
	// (RREQ_DUAL packs a channel into arg, RADIO_DUAL_VFO in bit 31: INT32_MIN can't be negated, and isn't a step)
	int steps = req->arg > 0 ? req->arg : req->arg != INT32_MIN ? -req->arg : INT32_MAX;

	if (!alert) {
		activity(sm, now_ms);
	}
	if (!alert && req->type != RREQ_DMR_FOCUS && req->type != RREQ_SETTINGS && req->type != RREQ_ROAM) {
		roam_cancel(sm, now_ms);	// the user does something: back to the site a roaming search started from
	}
#if CONFIG_SCAN && CONFIG_DUAL_WATCH
	// The selected channel goes into a VFO the other channel isn't on: the one last used, else the other one. Chosen
	// while the pair is known, used once the stop has the receiver back on the selected channel
	bool to_vfo = req->type == RREQ_VFO_COPY || req->type == RREQ_VFO_SET || (req->type == RREQ_VFO && req->arg != 0);
	int vfo_pick = -1;
	if (to_vfo && dual_running(sm) && !sm->dual[sm->dual_sel].vfo) {
		const radio_dual_ref_t *o = &sm->dual[!sm->dual_sel];
		vfo_pick = o->vfo && o->pos == sm->vfo_idx ? !sm->vfo_idx : sm->vfo_idx;
	}
#endif
	if (retunes(req->type)) {
		scan_stop(sm, dual_running(sm), now_ms);	// dual watch: the change starts from the selected one
	}
#if CONFIG_SCAN && CONFIG_DUAL_WATCH
	if (vfo_pick >= 0) {
		vfo_use(sm, (uint8_t)vfo_pick);
	}
#endif
#if CONFIG_SCAN && CONFIG_DUAL_WATCH
	if (for_selected(req->type) && dual_running(sm)) {
		dual_to_selected(sm, now_ms);
	}
#endif
	switch (req->type) {
	case RREQ_CH_STEP:
		if (busy || steps == 0) {
			break;
		}
		for (int i = 0; i < steps; i++) {
			if (sm->view.vfo) {
				vfo_step(sm, dir, now_ms);
			} else {
				channel_step(sm, dir, now_ms);
			}
		}
		break;
	case RREQ_VFO_STEP:
		if (busy || !sm->view.vfo) {
			break;
		}
		for (int i = 0; i < steps; i++) {
			vfo_step(sm, dir, now_ms);
		}
		break;
	case RREQ_ZONE_POS: {
		uint16_t pos;
		channel_t ch;
		if (busy || !sm->have_channels || req->arg < 0 || req->arg >= sm->zone.count) {
			break;
		}
		pos = (uint16_t)req->arg;
		if (codeplug_channel(sm->zone.channels[pos], &ch)) {
			set_channel(sm, pos, &ch);
			tune(sm, now_ms);
		}
		break;
	}
	case RREQ_ZONE: {
		zone_t z;
		uint16_t pos;
		channel_t ch;
		if (busy || req->arg < 0 || req->arg >= codeplug_zone_count()) {
			break;
		}
		if (read_zone((uint16_t)req->arg, &z) && find_channel(&z, 0, 1, &pos, &ch)) {
			set_zone(sm, (uint16_t)req->arg, &z);
			set_channel(sm, pos, &ch);
			sm->have_channels = true;
			tune(sm, now_ms);
		}
		break;
	}
	case RREQ_VFO:
		if (busy || !VFO_AVAILABLE || (req->arg != 0) == sm->view.vfo) {
			break;
		}
		if (req->arg != 0) {
			enter_vfo(sm, now_ms);
		} else {
			leave_vfo(sm, now_ms);
		}
		break;
	case RREQ_VFO_COPY:
		if (!busy) {
			vfo_copy(sm, now_ms);
		}
		break;
	case RREQ_VFO_SET:
		if (busy || !VFO_AVAILABLE || req->arg <= 0 || !board_in_band((uint32_t)req->arg, false)) {
			break;
		}
		vfo_set_hz(sm, (uint32_t)req->arg);
		if (sm->view.vfo) {
			tune(sm, now_ms);
		} else {
			enter_vfo(sm, now_ms);
		}
		break;
	case RREQ_VFO_TX: {
		// The receiver stays as it is: TX is set up at the next key-up. DMR works out repeater or direct from the
		// split, so the call stack is set up again (it publishes the view)
		uint32_t tx = req->arg > 0 ? (uint32_t)req->arg : sm->vfo_ch.rx_hz;
		if (busy || !sm->view.vfo || req->arg < 0 || !board_in_band(tx, true)) {
			break;
		}
		sm->vfo_ch.tx_hz = tx;
		sm->view.ch.tx_hz = tx;
		changed(sm);
		break;
	}
	case RREQ_PTT:
#if CONFIG_FM_TX_TONES
		if (sm->burst_keyed) {
			sm->burst_keyed = req->arg == 0;	// PTT pressed during a burst takes the transmission over
			break;
		}
#endif
		if (req->arg != 0) {
			if (!sm->ptt) {
				sm->host_audio = req->arg == 2;	// a transmission keeps the source it started with
			}
			ptt_down(sm, now_ms);
		} else {
			ptt_up(sm, now_ms);
		}
		break;
	case RREQ_MONITOR:
#if CONFIG_SCAN && CONFIG_DUAL_WATCH
		// Before the DMR test: the receiver may be on the other channel, of the other mode
		if (req->arg == 1 && dual_running(sm)) {
			dual_to_selected(sm, now_ms);
		}
#endif
		if (sm->view.monitor != (req->arg == 1)) {
			sm->view.monitor = req->arg == 1;
			outputs(sm);
			changed(sm);
		}
		break;
	case RREQ_POWER: {
		int n = board.power_count;
		if (n == 0 || steps == 0) {
			break;
		}
		int p = ((sm->view.power + req->arg) % n + n) % n;
		sm->view.power = (uint8_t)p;
		sm->temp_power = sm->view.power != channel_power(&sm->view.ch);
		set_temporary(sm);
		changed(sm);
		break;
	}
	case RREQ_VOLUME:
		sm->vol_step = (uint8_t)(req->arg < 0 ? 0 : req->arg > RADIO_VOLUME_MAX ? RADIO_VOLUME_MAX : req->arg);
		break;
	case RREQ_VOLUME_OVERRIDE:
		sm->vol_override = (int8_t)(req->arg < 0 ? -1 : req->arg > RADIO_VOLUME_MAX ? RADIO_VOLUME_MAX : req->arg);
		break;
	case RREQ_SQUELCH: {
		int s = sm->view.squelch + req->arg;
		if (s < 0) {
			s = 0;
		} else if (s > RADIO_SQUELCH_MAX) {
			s = RADIO_SQUELCH_MAX;
		}
		if (s != sm->view.squelch) {
			sm->view.squelch = (uint8_t)s;
			sm->temp_squelch = sm->view.squelch != sm->base_squelch;
			set_temporary(sm);
			changed(sm);
		}
		break;
	}
	case RREQ_CLEAR_TEMP:
		if (sm->view.temporary) {
			if (clear_temporary(sm)) {
				if (busy) {
					sm->retune_pending = true;
				} else {
					tune(sm, now_ms);
				}
			}
			changed(sm);
		}
		break;
	case RREQ_RETUNE:	// a board that applies its reference trim when it tunes (hal_radio_t.ref_trim)
		sm->retune_due = true;	// radio_sm_poll: once the trim stops changing
		sm->retune_due_ms = now_ms + RADIO_RETUNE_SETTLE_MS;
		break;
	case RREQ_BANDWIDTH:
		if (busy || sm->view.ch.mode != RADIO_MODE_FM || (req->arg != RADIO_BW_12K5 && req->arg != RADIO_BW_25K) ||
				req->arg == sm->view.ch.bw) {
			break;
		}
		sm->view.ch.bw = (uint8_t)req->arg;
		if (sm->view.vfo) {
			sm->vfo_ch.bw = sm->view.ch.bw;
		}
		set_temporary(sm);
		tune(sm, now_ms);
		break;
	case RREQ_TONE: {
		uint8_t which = (uint8_t)(((uint32_t)req->arg >> 24) & 3u);
		subtone_t t = { (uint8_t)(((uint32_t)req->arg >> 16) & 0xFFu), (uint16_t)req->arg };
		if (t.type == TONE_NONE) {
			t.value = 0;
		}
		if (busy || sm->view.ch.mode != RADIO_MODE_FM || which == 0 || !tones_valid(&t)) {
			break;
		}
		bool rx_changed = (which & RADIO_TONE_RX) && !same_tone(&t, &sm->view.ch.rx_tone);
		bool tx_changed = (which & RADIO_TONE_TX) && !same_tone(&t, &sm->view.ch.tx_tone);
		if (which & RADIO_TONE_TX) {
			sm->view.ch.tx_tone = t;
		}
		if (which & RADIO_TONE_RX) {
			sm->view.ch.rx_tone = t;
		}
		if (sm->view.vfo) {
			sm->vfo_ch.tx_tone = sm->view.ch.tx_tone;
			sm->vfo_ch.rx_tone = sm->view.ch.rx_tone;
		}
		set_temporary(sm);
		if (rx_changed) {
			tune(sm, now_ms);	// the receiver's tone detector
		} else if (tx_changed) {
			changed(sm);		// used on the next PTT
		}
		break;
	}
	case RREQ_MODE:
		if (busy || req->arg == sm->view.ch.mode ||
				(sm->view.vfo ? !radio_vfo_mode_built(req->arg) : (req->arg != RADIO_MODE_FM && req->arg != RADIO_MODE_DMR)) ||
				(req->arg == RADIO_MODE_FM && !CONFIG_FM) ||
				(req->arg != RADIO_MODE_FM && listen_band(sm->view.ch.rx_hz))) {
			break;
		}
		sm->view.ch.mode = (uint8_t)req->arg;
		if (codeplug_mode_digital_voice(sm->view.ch.mode)) {
			sm->view.ch.nac_rx = sm->view.ch.nac_tx = req->arg == RADIO_MODE_M17 ? 0 : CODEPLUG_NAC_DEFAULT;
			sm->view.ch.talkgroup = 1;
			sm->view.ch.bw = RADIO_BW_12K5;
		}
		if (sm->view.ch.mode == RADIO_MODE_DMR && sm->view.ch.timeslot != 2) {
			sm->view.ch.timeslot = 1;
		}
		if (sm->view.vfo) {
			sm->vfo_ch.mode = sm->view.ch.mode;	// the VFO's own mode, not a temporary change
			sm->vfo_ch.timeslot = sm->view.ch.timeslot;
			sm->vfo_ch.nac_rx = sm->view.ch.nac_rx;
			sm->vfo_ch.nac_tx = sm->view.ch.nac_tx;
			sm->vfo_ch.talkgroup = sm->view.ch.talkgroup;
			sm->vfo_ch.bw = sm->view.ch.bw;
		}
		set_temporary(sm);
		tune(sm, now_ms);	// the call stack starts or stops with the mode
		break;
	case RREQ_POWER_OFF:
		if (req->arg != 0) {
			// Still running after all (auto power off with the knob on): back to normal, registration kept
			sm->power_off_req = false;
			sm->power_off_busy = false;
			break;
		}
		sm->power_off_req = true;
		sm->power_off_busy = CONFIG_DMR_DATA_CORE;	// data_tick clears it once the deregistration is out
		break;
	case RREQ_RELOAD:
#if CONFIG_APRS
		aprs_reload();
#endif
		if (busy || sm->ptt || ZONE_WRITE_BUSY()) {
			sm->reload_pending = true;	// after the transmission or the hotspot (radio_sm_poll)
		} else {
			reload(sm, now_ms);
		}
		break;
#if CONFIG_SCAN
	case RREQ_SCAN:
		scan_request(sm, req, now_ms);
		break;
#if CONFIG_DUAL_WATCH
	case RREQ_DUAL:
		dual_request(sm, req, now_ms);
		break;
#endif
	case RREQ_SCAN_VFO:
		if (!busy && !sm->ptt) {
			scan_start(sm, SCAN_VFO, (uint32_t)req->arg, req->arg2, now_ms);
		}
		break;
#endif
	case RREQ_BEEP:
		if (req->arg != BEEP_KEY || sm->set.key_beep) {
			beep(sm, (uint8_t)req->arg);
		}
		break;
#if CONFIG_FM_TX_TONES
	case RREQ_TONE_BURST:
		tone_burst(sm, req->arg != 0, req->arg2, now_ms);
		break;
#endif
#if CONFIG_DTMF
	case RREQ_DTMF:
		dtmf_key(sm, (char)(req->arg & 0xFF), (req->arg & 0x100) != 0, now_ms);
		break;
#endif
	default:
		break;
	}
	persist(sm, now_ms);
}

bool radio_volume_soft(void)
{
	return board.radio != NULL && board.radio->volume != NULL && board.power != NULL &&
	       board.power->volume != NULL && board.power->volume() < 0;
}

bool radio_volume_settable(void)
{
	return board.radio != NULL && board.radio->volume != NULL;
}

uint8_t radio_volume_percent(uint8_t step)
{
	return (uint8_t)((step * 100u + RADIO_VOLUME_MAX / 2u) / RADIO_VOLUME_MAX);
}

// The stored byte: 0 = the default; bit 7 set = step + 1 on this scale; 1..11 = a step 0..10 stored by the first
// version, scaled to this range (docs/spec/settings-store.md 3)
uint8_t radio_volume_step(uint8_t stored)
{
	if (stored == 0) {
		return RADIO_VOLUME_DEFAULT;
	}
	if (stored & 0x80u) {
		unsigned n = stored & 0x7Fu;
		return (uint8_t)(n == 0 || n - 1u > RADIO_VOLUME_MAX ? RADIO_VOLUME_MAX : n - 1u);
	}
	unsigned old = stored > 11 ? 10u : stored - 1u;
	return (uint8_t)((old * RADIO_VOLUME_MAX + 5u) / 10u);
}

uint8_t radio_beep_level(uint8_t cap, int16_t volume_percent)
{
	if (volume_percent < 0 || volume_percent >= cap) {
		return cap;
	}
	return (uint8_t)volume_percent;
}

uint8_t radio_volume_stored(uint8_t step)
{
	return (uint8_t)(0x80u | ((step > RADIO_VOLUME_MAX ? RADIO_VOLUME_MAX : step) + 1u));
}

// Follow a volume knob the MCU reads (soft volume): pass changes of 2 % or more, and the ends exactly. A radio with
// no knob (volume() = -1) gets the stored step instead, and a knob radio the debug link's override
// (radio-features.md 4.2)
static void follow_volume(radio_sm_t *sm)
{
	if (board.radio->volume == NULL || board.power == NULL || board.power->volume == NULL) {
		return;
	}
	int v = board.power->volume();
	uint8_t src = RVOL_KNOB;
	if (v < 0) {
		v = radio_volume_percent(sm->vol_step);
		src = RVOL_SOFT;
	} else if (sm->vol_override >= 0) {
		v = radio_volume_percent((uint8_t)sm->vol_override);
		src = RVOL_OVERRIDE;
	}
	if (src != sm->view.volume_src || sm->vol_step != sm->view.volume_step ||
	    sm->vol_override != sm->view.volume_override) {
		sm->view.volume_src = src;
		sm->view.volume_step = sm->vol_step;
		sm->view.volume_override = sm->vol_override;
		changed(sm);
	}
	int d = v - sm->volume_sent;
	if (sm->volume_sent < 0 || d >= 2 || d <= -2 || ((v == 0 || v == 100) && v != sm->volume_sent)) {
		sm->volume_sent = (int16_t)v;
		sm->view.volume = (int8_t)v;
		changed(sm);
		board.radio->volume((uint8_t)v);
	}
}

// Battery monitor: a reading a second, none during or just after a transmission. A radio that measures its supply
// instead gets the reading a second for display, and nothing acts on it: no thresholds are defined for a supply
static void battery_poll(radio_sm_t *sm, uint32_t now_ms)
{
	bool tx = sm->view.state == RS_TX;
	if (tx) {
		sm->tx_end_ms = now_ms;
		return;
	}
	if (board_volts() == BOARD_VOLTS_SUPPLY) {
		if (now_ms - sm->batt_ms >= RADIO_BATT_PERIOD_MS) {
			sm->batt_ms = now_ms;
			sm->view.supply_mv = board.power->supply_mv();
		}
		return;
	}
	if (!board_has_battery() || board.power == NULL || board.power->battery_mv == NULL || now_ms - sm->batt_ms < RADIO_BATT_PERIOD_MS ||
			(sm->tx_end_ms != 0 && now_ms - sm->tx_end_ms < RADIO_BATT_TX_QUIET_MS)) {
		return;
	}
	sm->batt_ms = now_ms;
	uint16_t mv = board.power->battery_mv();
	uint8_t pct = board.power->battery_pct != NULL ? board.power->battery_pct() : 0;
	batt_event_t ev = batt_update(&sm->batt, mv, board.battery_low_mv, board.battery_critical_mv, now_ms);
	sm->view.battery_mv = mv;
	if (sm->view.battery != sm->batt.level || sm->view.battery_pct != pct) {
		sm->view.battery = sm->batt.level;
		sm->view.battery_pct = pct;
		changed(sm);
	}
	if (ev == BATT_EV_LOW) {
		beep(sm, BEEP_BATTERY_LOW);
	} else if (ev == BATT_EV_CRITICAL) {
		beep(sm, BEEP_BATTERY_CRIT);
	}
	bool off = sm->set.battery_off_s != 0 &&
		batt_critical_for(&sm->batt, now_ms) >= (uint32_t)sm->set.battery_off_s * 1000u;
	if (off != sm->view.power_off) {
		sm->view.power_off = off;
		changed(sm);
	}
}

#if CONFIG_BATTERY_SAVE
static bool save_allowed(const radio_sm_t *sm, uint32_t now_ms)
{
	if (sm->set.rx_save == 0 || board.radio->rx_sleep == NULL || sm->view.ch.mode != RADIO_MODE_FM || sm->hotspot ||
			sm->view.state != RS_IDLE || sm->view.monitor || sm->ptt || sm->view.ch.no_eco || sm->view.bcast) {
		return false;
	}
#if CONFIG_APRS
	if (sm->aprs_phase != APRS_RF_OFF || aprs_rf_pending() || aprs_rx_listening()) {
		return false;	// a sleeping receiver misses packets
	}
#endif
#if CONFIG_SCAN
	if (scan_on(&sm->scan)) {
		return false;
	}
#endif
	return now_ms - sm->activity_ms >= (uint32_t)sm->set.rx_save_idle_s * 1000u;
}

// Receiver duty cycling while idle on an FM channel. True: skip this poll's status and squelch (asleep, or the
// readings right after waking)
static bool save_poll(radio_sm_t *sm, uint32_t now_ms)
{
	if (!save_allowed(sm, now_ms)) {
		if (sm->save_on) {
			if (sm->save_asleep) {
				board.radio->rx_sleep(false);
			}
			sm->save_on = sm->save_asleep = false;
			sm->view.rx_sleeping = false;
			changed(sm);
		}
		return false;
	}
	if (!sm->save_on) {
		sm->save_on = true;
		sm->save_asleep = false;
		sm->save_ms = now_ms;
	}
	uint32_t t = now_ms - sm->save_ms;
	if (sm->save_asleep) {
		uint32_t sleep_ms = (uint32_t)RADIO_SAVE_SLEEP_MS << (sm->set.rx_save - 1u);	// x1, x2, x4
		if (t >= sleep_ms) {
			board.radio->rx_sleep(false);
			sm->save_asleep = false;
			sm->save_ms = now_ms;
			sm->view.rx_sleeping = false;
			changed(sm);
		}
		return true;
	}
	if (t < RADIO_SAVE_SETTLE_MS) {
		return true;
	}
	if (t >= RADIO_SAVE_WAKE_MS) {
		board.radio->rx_sleep(true);
		sm->save_asleep = true;
		sm->save_ms = now_ms;
		sm->view.rx_sleeping = true;
		changed(sm);
		return true;
	}
	return false;
}
#endif

// Polls of noise above the line before the squelch closes
static uint8_t close_polls(void)
{
#if CONFIG_APRS_RX
	if (aprs_rx_listening()) {
		return RADIO_SQL_CLOSE_POLLS_APRS;
	}
#endif
	return RADIO_SQL_CLOSE_POLLS;
}


static int16_t rssi_held(radio_sm_t *sm, int16_t rssi_dbm)
{
	(void)sm;
	return rssi_dbm;
}

void radio_sm_poll(radio_sm_t *sm, uint32_t now_ms)
{
	radio_status_t st;
	// codeplug_serial() before zone_edit_busy(): a zone write that moved indexes has its log entry by then
	if ((sm->reload_pending || codeplug_serial() != sm->codeplug_serial || ZONES_MOVED(sm)) && !ZONE_WRITE_BUSY() &&
	    !sm->hotspot && !sm->ptt && sm->view.state != RS_TX) {
		reload(sm, now_ms);
		persist(sm, now_ms);
	}
	follow_volume(sm);
	battery_poll(sm, now_ms);
#if CONFIG_BEEP
	bool shared = CONFIG_M17 && sm->dv_running;
	beep_service(shared);
#endif
#if CONFIG_APRS_RX
	audio_mux_service(now_ms);	// the stream for a host audio session or APRS (after the beep: it may have stopped it)
#endif
#if CONFIG_APRS
	aprs_poll(sm, now_ms);	// before battery saving: beacons are due while the receiver sleeps too
#endif
	// The reference trim changed (RREQ_RETUNE): one tune once it has held still, so stepping it drops a reception once,
	// not at every step. It waits for an APRS job; while transmitting or a hotspot, for the end of it
	if (sm->retune_due && (int32_t)(now_ms - sm->retune_due_ms) >= 0) {
		bool aprs = false;
#if CONFIG_APRS
		aprs = sm->aprs_phase != APRS_RF_OFF;
#endif
		if (sm->view.state == RS_TX || sm->hotspot) {
			sm->retune_due = false;
			sm->retune_pending = true;
		} else if (!aprs) {
			tune(sm, now_ms);
		}
	}
#if CONFIG_BATTERY_SAVE
	if (save_poll(sm, now_ms)) {
		outputs(sm);
		return;
	}
#endif
	// An APRS job has the channel receiver on its own frequency in FM: its carrier sense reads that receiver as is
	bool own_rx = true;
#if CONFIG_APRS
	own_rx = sm->aprs_phase == APRS_RF_OFF;
#endif
	(void)own_rx;
	board.radio->status(&st);
#if RADIO_SM_DV
	sm->dv_now_ms = now_ms;
#endif
	sm->view.rssi_dbm = rssi_held(sm, st.rssi_dbm);
	sm->view.noise = st.noise;
	int drift = sm->view.rssi_dbm - sm->rssi_published;
	if (drift >= RADIO_RSSI_DEADBAND_DB || drift <= -RADIO_RSSI_DEADBAND_DB) {
		sm->rssi_published = sm->view.rssi_dbm;
		changed(sm);
	}
#if CONFIG_APRS
	if (sm->aprs_phase != APRS_RF_OFF) {
		aprs_rf_service(sm, &st, now_ms);	// on the APRS frequency: no squelch, scan or time-out here
		outputs(sm);
		return;
	}
#endif
#if CONFIG_SCAN
	if (scan_on(&sm->scan) && !sm->ptt && sm->view.state != RS_TX && sm->view.state != RS_TX_DENIED && !sm->hotspot) {
		uint32_t steps = sm->scan.steps;
		scan_service(sm, &st, now_ms);
		if (sm->scan.steps != steps) {
			outputs(sm);
			return;		// retuned: this poll's readings belong to the last stop
		}
	}
#if CONFIG_DUAL_WATCH
	else if (dual_running(sm) && !sm->hotspot) {
		scan_talked(&sm->scan, now_ms);	// transmitting: the wait after it starts when it ends
	}
#endif
#endif
#if RADIO_SM_DV
	if (sm->dv_running) {
		outputs(sm);
		return;		// the state follows the native session (radio_sm_tick); its TX time-out is dv_follow's
	}
#endif

	if (sm->view.state == RS_TX) {
#if CONFIG_FM_TX_TONES
		if (sm->burst_end_ms != 0 && sm->view.tone_burst && (int32_t)(now_ms - sm->burst_end_ms) >= 0) {
			tone_burst(sm, false, 0, now_ms);	// its "off" never came: unkey as the off would
			if (sm->view.state != RS_TX) {
				return;
			}
		}
		if (sm->roger_ms != 0) {
			roger_poll(sm, now_ms);
			if (sm->view.state != RS_TX) {
				return;
			}
		}
#endif
#if CONFIG_SUBTONE
		if (sm->tail_ms != 0) {
			tail_poll(sm, now_ms);
			if (sm->view.state != RS_TX) {
				return;
			}
		}
#endif
#if CONFIG_DTMF
		dtmf_poll(sm, now_ms);	// PTT is up during the tail: no digit sounds over it
#endif
		tx_timer(sm, now_ms);
		if (sm->view.tot_s != 0 && sm->view.tx_elapsed_ms >= (uint32_t)sm->view.tot_s * 1000) {
			dtmf_cancel(sm, now_ms);
#if CONFIG_FM_TX_TONES
			if (sm->roger_ms != 0 || sm->view.tone_burst) {
				tx_tone(0);
				sm->roger_ms = 0;
				sm->view.roger = false;
				sm->view.tone_burst = false;
			}
#endif
#if CONFIG_SUBTONE
			tail_cancel(sm);
#endif
			board.radio->tx_off();
			deny(sm, TXDENY_TIMEOUT);	// until PTT is released
			if (!sm->ptt) {
				fm_tx_end(sm, true, now_ms);	// the roger beep ran into the time-out: nothing holds the deny
			}
		}
		return;
	}
	if (sm->view.state == RS_TX_DENIED) {
		return;
	}

	// Open on carrier (noise below the level's threshold) and the RX tone, both; the monitor overrides (outputs())
	follow_tone(sm, st.tone_ok);
	uint8_t sq = sm->view.squelch;
	uint8_t open_below = radio_sm_squelch_open_below(sq);
	bool listening = bcast_listening(sm);
	bool opens = (listening || (sm->view.tone_ok && (sq == 0 || st.noise < open_below))) && !dual_peeking(sm);
	bool holds = listening || (sm->view.tone_ok && (sq == 0 || st.noise < open_below + RADIO_SQL_HYST_DB));

	if (sm->view.state == RS_IDLE) {
		if (opens) {
			sm->view.state = RS_RX;
			sm->close_count = 0;
			changed(sm);
		}
	} else if (holds) {
		activity(sm, now_ms);
		sm->close_count = 0;
	} else if (++sm->close_count >= close_polls()) {
		end_reception(sm, now_ms);
		changed(sm);
	}
	outputs(sm);
}


void radio_sm_tick(radio_sm_t *sm, uint32_t now_ms)
{
#if RADIO_SM_CW
	cw_service(sm, now_ms);
#endif
#if RADIO_SM_DV
	sm->dv_now_ms = now_ms;
	if (sm->dv_running) {
		radio_dv_service(now_ms);	// NXDN's modem runs here
		dv_permit_poll(sm, now_ms);
		dv_follow(sm, now_ms);
	}
#endif
	(void)sm;
	(void)now_ms;
}
