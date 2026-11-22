// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The radio core: owns the channel / VFO state, squelch, PTT and the TX timer, and drives board.radio.
// It runs in its own task. The UI reads a snapshot and posts requests; it never touches the hardware.
#ifndef RADIO_CORE_H
#define RADIO_CORE_H

#include <stdint.h>
#include <stdbool.h>
#include "codeplug/codeplug.h"
#include "config.h"

void radio_req_mode(uint8_t mode);
void radio_req_vfo_can(uint8_t can);

// Conventional modes available to a VFO; trunking still needs a programmed channel.
static inline bool radio_vfo_mode_built(int mode)
{
	return (mode == RADIO_MODE_FM && CONFIG_VFO_FM) || (mode == RADIO_MODE_DMR && CONFIG_VFO_DMR) ||
		((CONFIG_VFO_FM || CONFIG_VFO_DMR) && ((mode == RADIO_MODE_P25 && CONFIG_P25 && CONFIG_P25_NATIVE) ||
		(mode == RADIO_MODE_M17 && CONFIG_M17)));
}

typedef enum {
	RS_IDLE,	// receiving, squelch closed
	RS_RX,		// receiving, squelch open (carrier and tone)
	RS_TX,
	RS_TX_DENIED,	// PTT held but transmit refused (RX-only, out of band, timed out); shown until PTT release
} radio_state_t;

// TXDENY_MODE: the channel's mode is not built in (a DMR channel in an FM-only build, or the reverse).
// DMR: NO_CODEC no vocoder merged (receive only), NO_DMR the modem didn't start, BUSY a call on our slot (polite
// access), NO_REPEATER the repeater didn't wake, REJECTED the modem refused, HOTSPOT the radio is a hotspot now,
// NO_ID no DMR ID in the codeplug, SFR_RPT the radio is the channel's single frequency repeater. BUSY on FM: busy channel lockout. INHIBIT: transmit is switched off in the
// settings. BATTERY: the battery is critical. AM: the channel is received in AM (the air band, CONFIG_AM_RX): receive
// only, the radio never transmits there. BCAST: the same for the broadcast band (CONFIG_BCAST_FM)
typedef enum {
	TXDENY_NONE, TXDENY_RX_ONLY, TXDENY_BAND, TXDENY_TIMEOUT, TXDENY_NO_FREQ, TXDENY_MODE,
	TXDENY_NO_CODEC, TXDENY_NO_DMR, TXDENY_BUSY, TXDENY_NO_REPEATER, TXDENY_REJECTED, TXDENY_HOTSPOT, TXDENY_NO_ID,
	TXDENY_INHIBIT, TXDENY_BATTERY, TXDENY_AM, TXDENY_BCAST, TXDENY_SFR_RPT,
} tx_deny_t;

// Busy channel lockout (FM): CARRIER refuses PTT while anything is heard on the channel; WRONG_TONE only while a
// carrier without our RX tone is heard (the channel is used by another group)
typedef enum { RADIO_BCL_OFF, RADIO_BCL_CARRIER, RADIO_BCL_WRONG_TONE } radio_bcl_t;
// VOX: off, on channels whose codeplug entry allows it, on every FM channel
typedef enum { RADIO_VOX_OFF, RADIO_VOX_CHANNEL, RADIO_VOX_ALL } radio_vox_t;

// The radio core's settings (docs/spec/radio-features.md). The radio core loads them from the settings store at
// start-up (docs/spec/settings-store.md 3) and saves every change made with radio_set_settings
typedef struct {
	uint16_t tot_s;		// the radio's TX time-out, for channels whose own is 0; 0 = none
	uint8_t tot_warn_s;	// warning beep this long before the time-out, 0 = none
	uint8_t bcl;		// radio_bcl_t
	bool roger_beep;	// FM: a short tone pair at the end of each transmission (CONFIG_FM_TX_TONES)
	bool tx_inhibit;	// refuse every transmission
	// Scan (CONFIG_SCAN)
	uint8_t scan_resume;	// scan_resume_t (radio/scan.h): 0 time, 1 carrier, 2 stop
	uint8_t scan_hold_s;	// time mode: move on after this long on a signal
	uint16_t scan_delay_ms;	// after the signal ends, move on after this long
	uint16_t scan_dwell_ms;	// listen time per channel with nothing heard
	bool scan_priority;	// zone scan: revisit the start channel between the others
	bool scan_beep;		// a short beep when the scan stops on a signal
	uint16_t dual_delay_ms;	// dual watch on one receiver: after a signal ends, stay on its channel this long (the
				// reply), looking at the other one in between
	// Beeps (CONFIG_BEEP)
	uint8_t beep_level;	// 0 = silent .. 100: the loudest a beep gets (it follows the volume up to this)
	bool key_beep;		// radio_req_beep(BEEP_KEY) sounds
	bool talk_permit;	// the talk permit tone (DMR, P25, NXDN; channels without no_beep)
	// VOX (CONFIG_VOX)
	uint8_t vox;		// radio_vox_t
	uint8_t vox_level;	// sensitivity 1 (needs a loud voice) .. 10
	uint16_t vox_tail_ms;	// stays keyed this long after the voice stops
	// Power
	uint8_t rx_save;	// receiver battery saving (CONFIG_BATTERY_SAVE): 0 off, 1..3 = sleep 1, 2, 4 x the listen time
	uint16_t rx_save_idle_s;	// starts after this long without activity
	uint16_t battery_off_s;	// switch off after this long with the battery critical, 0 = never
	uint16_t backlight_dim_s;	// for batt_backlight_pct (radio/battery.h): dim after, 0 = never
	uint16_t backlight_off_s;	// off after, 0 = never
	// DMR (CONFIG_DMR)
	uint8_t reply_s;	// reply window after a call, 0 = off .. 30 s, RADIO_REPLY_CODEPLUG = the codeplug's hang times
	// DMR repeater roaming (CONFIG_DMR_ROAMING; docs/spec/dmr-roaming.md). Which zones roam is kept apart (RREQ_ROAM)
	int8_t roam_rssi_dbm;	// threshold: the site counts as weak below it, a candidate must reach it (-125 .. -60)
	uint8_t roam_hyst_db;	// a candidate must beat the current site by this much (1 .. 30)
	uint8_t roam_probe;	// 0 passive, 1 wake probe (a BS_Dwn_Act on each silent site visited: transmits)
	uint16_t roam_check_s;	// periodic search: look at the other sites this often, 0 = off (30 .. 3600)
	uint16_t roam_beacon_s;	// the repeaters' idle beacon interval, 0 = none (10 .. 600); must match the repeaters
	uint8_t roam_beacon_len_s;	// their beacon duration (1 .. 10)
	uint8_t roam_survey;	// 0 off, 1 the sites beacon together (one window), 2 each on its own schedule
	bool rpt_beacon;	// our repeater roles (SFR) key an idle beacon on that interval and duration
	uint8_t sfr_rx;		// SFR user channels hear: dmr_sfr_rx_t (0 the repeat only, 1 both first arrival, 2 dual-path RX)
	uint8_t sfr_cwid;	// our SFR repeater's CW ID interval: 0 off, 1 5 min, 2 10, 3 15, 4 30 (CONFIG_SFR_CWID)
	// Broadcast FM (CONFIG_BCAST_FM)
	uint8_t bcast_step;	// the VFO's automatic step there: RADIO_BCAST_STEP_200_ODD or RADIO_BCAST_STEP_100
} radio_settings_t;

// radio_settings_t.bcast_step: 200 kHz between the odd tenths (88.1, 88.3 MHz: the broadcast channels of the
// Americas), or every 100 kHz. Either way a frequency typed in may be any 100 kHz point
#define RADIO_BCAST_STEP_200_ODD	0u
#define RADIO_BCAST_STEP_100		1u

#define RADIO_REPLY_CODEPLUG	0xFFu	// radio_settings_t.reply_s: the codeplug's hang times (else the stack's 10 s)
// radio_settings_t.dual_delay_ms: the default and the most. In the wait the receiver looks at the other channel for a
// scan dwell after each dwell back on the signal's
#define RADIO_DUAL_DELAY_MS	1000u
#define RADIO_DUAL_DELAY_MAX_MS	10000u

typedef enum { RADIO_BATT_OK, RADIO_BATT_LOW, RADIO_BATT_CRITICAL } radio_batt_t;	// = batt_level_t

// DTMF (radio-features.md 3.7)
#define RADIO_DTMF_MIN_MS	100	// shortest tone: a quick tap still decodes
#define RADIO_DTMF_GAP_MS	60	// shortest pause between two digits: the same digit twice decodes as two
#define RADIO_DTMF_SHOWN	8	// digits of this transmission the view keeps (the TX screen shows them)


// What a digital voice channel (P25 on the native modem; radio_dv.h) is doing, for the screens
typedef enum { RDV_IDLE, RDV_RX, RDV_RX_OTHER, RDV_TX } radio_dv_state_t;
typedef struct {
	bool active;		// the channel's receiver runs
	bool built;		// this build carries the channel's digital mode (radio_sm's dv_mode_built)
	bool fault;		// its session faulted and the channel re-arms it
	uint8_t state;		// radio_dv_state_t
	uint16_t nac;		// NAC of the call (RX) or ours (TX)
	uint16_t talkgroup;	// the call's talkgroup, or ours while transmitting
	char callsign[10],destination[10];
	uint32_t source;	// the caller's unit ID, or ours while transmitting
	bool encrypted;		// the call heard is encrypted: muted, never decrypted
	bool emergency;
	uint32_t call_ms;	// hal_millis() when the call (RX or TX) began
	// The last call heard
	uint16_t last_talkgroup;
	uint32_t last_source;
	uint32_t last_ms;	// hal_millis() of its end, 0 = none
	// P25 trunking channels: where the radio is on the system (radio_dv_trunk_t) and whether the system took it
	uint8_t trunk;
	bool trunk_registered;
} radio_dv_view_t;
// The trunking states, in the order the trunking logic numbers them (core/p25t/su.h)
typedef enum { RDV_TRUNK_HUNT, RDV_TRUNK_SITE, RDV_TRUNK_CALL, RDV_TRUNK_REQUEST, RDV_TRUNK_QUEUED, RDV_TRUNK_TALK } radio_dv_trunk_t;


#define RADIO_LED_GREEN		0x01u	// radio_view_t.leds
#define RADIO_LED_RED		0x02u

typedef struct {
	radio_state_t state;
	uint8_t tx_deny;	// tx_deny_t, while RS_TX_DENIED
	uint8_t leds;		// RADIO_LED_* bits: what the radio core drives its LEDs to (board.power->led)
	bool vfo;		// tuning a frequency by hand rather than a channel
	uint8_t vfo_index;	// which VFO: 0, or 1 the second (dual watch gives each channel its own)
	uint16_t zone_index;
	uint16_t zone_pos;	// position in the zone, 0-based
	uint16_t zone_size;
	uint16_t channel_number;	// codeplug channel number, 0 in VFO
	char zone_name[CODEPLUG_NAME_LEN + 1];
	channel_t ch;		// what the radio is set to now (VFO: the VFO settings, with ch.name "VFO")
	uint8_t power;		// effective board power level index
	bool temporary;		// a temporary change (P7) is in effect on this channel
	uint8_t temp;		// which settings are temporary changes: RADIO_TEMP_* bits (temporary == (temp != 0))
	uint8_t squelch;	// 0 = open .. 9 = tight
	bool monitor;		// squelch held open by the user
	bool tone_ok;		// the RX tone or code is heard (debounced; true when the channel has no RX tone)
	int16_t rssi_dbm;
	uint8_t noise;
	uint32_t tx_elapsed_ms;	// time in TX, while RS_TX
	bool tx_auto;		// that TX is the radio's own, not the user's: a data list (ARS, an ACK), a roaming probe, an
				// APRS beacon. It doesn't light the screen (radio_view_wakes)
	uint32_t last_tx_ms;	// hal_millis() when the last transmission began, 0 = none yet
	uint16_t tot_s;		// TX time-out, 0 = none
	uint32_t last_rx_ms;	// hal_millis() at the end of the last reception, 0 = none yet
	uint32_t vfo_step_hz;	// the VFO's step in use: as set, or with vfo_step_auto the width's (12.5 or 25 kHz)
	bool vfo_step_auto;
	codeplug_source_t source;
	bool hotspot;		// the radio is an MMDVM hotspot: the host link drives the modem, channel keys are off
	// Scan (CONFIG_SCAN; all 0 otherwise)
	uint8_t scan;		// scan_kind_t: 0 off, 1 zone, 2 VFO, 3 dual watch
	uint8_t scan_phase;	// scan_phase_t: listen, probe, hold, resume, paused
	uint8_t scan_skipped;	// nuisance-deleted stops
	uint16_t scan_probes;	// this scan: stops probed (a carrier at the end of the dwell)
	uint16_t scan_synced;	// ... stops where sync was heard (DMR: bursts with the channel's color code)
	bool rx2;		// dual watch hears both channels at once: the second receiver has the one not selected
	uint8_t dual_busy;	// dual watch on one receiver: bit i, channel i had a signal when the receiver last looked
				// at it during the wait after a call on the other (shown, not played)
	uint8_t dual_sel;	// dual watch: the selected channel as last asked for (0 A, 1 B); the UI follows a change
				// it didn't ask for (the debug link)
	uint8_t dual_stops;	// counts dual watch stops by radio_req_scan (stop, cancel, toggle): the UI keeps it off
	// FM extras
	bool tot_warning;	// TX: the time-out is near (tot_warn_s)
	bool tone_burst;	// TX: the 1750 Hz burst is on
	bool roger;		// TX: the roger beep is being sent after the PTT release
	// DTMF (CONFIG_DTMF; radio-features.md 3.7)
	bool dtmf_ready;	// a digit key would send its tone now (FM TX with PTT held, nothing else on the air)
	char dtmf;		// the digit sounding, 0 = none
	char dtmf_digits[RADIO_DTMF_SHOWN + 1];	// the last digits sent in this transmission (oldest first)
	uint16_t dtmf_sent;	// digits sent since start-up
	bool vox_keyed;		// TX keyed by VOX
	bool vox_on;		// VOX listens on this channel (the settings' vox, the channel, an FM channel)
	bool rx_sleeping;	// battery save: the receiver is off for now
	// Battery
	uint8_t battery;	// radio_batt_t
	uint8_t battery_pct;	// board.power->battery_pct(), 0 if unknown
	uint16_t battery_mv;
	uint16_t supply_mv;	// board.power->supply_mv() where the radio measures its supply (board_volts), else 0
	bool power_off;		// the radio core asks to be switched off (critical battery, battery_off_s)
	int8_t volume;		// percent last passed to hal_radio_t.volume (the knob, the override or the step), -1 = none yet
	uint8_t volume_src;	// radio_volume_src_t: where that level comes from
	uint8_t volume_step;	// the stored step (0..RADIO_VOLUME_MAX); in force when volume_src is RVOL_SOFT
	int8_t volume_override;	// the debug link's step in force over the knob (RVOL_OVERRIDE), -1 = none
	// Air band (CONFIG_AM_RX; docs/spec/airband-and-broadcast.md 3): the receiver demodulates AM on this channel or
	// VFO frequency (an FM channel in 108-137 MHz on a radio that can); receive only. noise then holds the AM
	// squelch's value from the RSSI (radio/radio_am.h)
	bool am;
	// Broadcast FM (CONFIG_BCAST_FM; airband-and-broadcast.md 4): an FM channel or the VFO in the broadcast
	// receiver's range plays on that receiver; receive only. noise and rssi_dbm then come from it (radio_bcast.h).
	// bcast_fault: the receiver didn't answer or refused the frequency
	bool bcast;
	bool bcast_fault;
	uint32_t bcast_khz;		// what the receiver reports it is tuned to, for the debug link (0 = not playing)
	radio_dv_view_t dv;	// digital voice, independent of DMR
} radio_view_t;

// view.temp bits
#define RADIO_TEMP_POWER	0x01u
#define RADIO_TEMP_SQUELCH	0x02u
#define RADIO_TEMP_BW		0x04u
#define RADIO_TEMP_TX_TONE	0x08u
#define RADIO_TEMP_RX_TONE	0x10u
#define RADIO_TEMP_TS		0x20u	// DMR timeslot (radio_req_dmr_timeslot)
#define RADIO_TEMP_CC		0x40u	// DMR color code (radio_req_dmr_color_code)
#define RADIO_TEMP_MODE		0x80u	// FM <-> DMR (radio_req_mode)

// Which tone radio_req_tone sets
#define RADIO_TONE_TX		0x01u
#define RADIO_TONE_RX		0x02u
#define RADIO_TONE_BOTH		(RADIO_TONE_TX | RADIO_TONE_RX)

void radio_core_start(void);
// A consistent copy of the current state
void radio_get_view(radio_view_t *v);
// Just the view's state, for a caller short of stack (the view is some 0.7 KB)
radio_state_t radio_get_state(void);
// Bumped on every visible change, so the UI can redraw only when needed
uint32_t radio_view_serial(void);

// Requests taken by the radio task, and lost to a full queue, since start-up
void radio_req_stats(uint32_t *handled, uint32_t *dropped);

// Requests (queued, handled by the radio task)
void radio_req_channel_step(int8_t dir);	// next / previous channel in the zone, wraps
void radio_req_zone_pos(uint16_t pos);		// jump to a position in the zone
void radio_req_zone(uint16_t index);
void radio_req_vfo(bool on);
void radio_req_vfo_step(int8_t dir);
void radio_req_vfo_set(uint32_t hz);		// RX frequency; TX keeps a split's offset where it can transmit, else = RX
void radio_req_vfo_tx(uint32_t hz);		// the VFO's TX frequency (split), 0 = simplex; in the VFO only
// Copy to VFO: what is in effect on the channel now (temporary changes too) becomes the VFO's own settings and the VFO
// opens; the channel is unchanged (docs/spec/dmr-voice.md 2.2). Nothing changes when radio_vfo_copy_check refuses
void radio_req_vfo_copy(void);
typedef enum {
	VFO_COPY_OK,
	VFO_COPY_NO_VFO,	// the build has no VFO
	VFO_COPY_NO_MODE,	// the channel's mode has no VFO in this build (CONFIG_VFO_FM / CONFIG_VFO_DMR)
	VFO_COPY_OUT_OF_BAND,	// a frequency outside the board's bands: the VFO can't hold it
	VFO_COPY_ALREADY,	// already in the VFO (the VFO is not a channel)
} radio_vfo_copy_t;
// Whether the VFO can take ch (the channel on view); implemented in radio_sm.c, safe to call from any task
radio_vfo_copy_t radio_vfo_copy_check(const channel_t *ch, bool in_vfo);
// The check on the shared view, then the request when it passes (the debug link); returns the check's answer
radio_vfo_copy_t radio_req_vfo_copy_checked(void);
// The VFO's step (Up / Down, VFO scan), kept in the settings: 1 kHz .. 1 MHz; RADIO_AM_STEP_833 (8333) is the exact
// 8.33 kHz air band grid (radio/radio_am.h); 0 = auto, the width's: 12.5 kHz, 25 kHz wide or in the air band
void radio_req_vfo_step_size(uint32_t hz);
void radio_req_ptt(bool down);
// PTT for a host audio session (USRP, docs/spec/usb-audio.md): the host's samples go on air instead of the microphone
// (FM: through the board's audio tap, refused without one; DMR: into the encoder, core/usbaudio/audio_mux.c). Up is
// radio_req_ptt(false)
void radio_req_ptt_host(bool down);
void radio_req_monitor(bool down);
// The monitor key released after its hold function ran (keyassign.h): FM closes the squelch, DMR doesn't toggle
void radio_req_monitor_held_up(void);
void radio_req_power(int8_t dir);		// temporary change of power level, wraps
void radio_req_squelch(int8_t dir);
void radio_req_clear_temporary(void);		// BACK on Home: revert temporary changes
void radio_req_retune(void);			// tune again now, or after the transmission (the reference trim changed)
void radio_req_reload(void);			// the settings' preferences changed (squelch, master power, DMR ID)
// FM bandwidth and sub-audio. On a channel these are temporary changes (P7); in the VFO they change the VFO itself.
// Refused on DMR channels, while transmitting, and for tones tones_valid() rejects
void radio_req_bandwidth(radio_bw_t bw);
void radio_req_tone(uint8_t which, subtone_t tone);	// which: RADIO_TONE_TX, RADIO_TONE_RX or RADIO_TONE_BOTH
// Scan (CONFIG_SCAN; no-ops otherwise). A zone scan starts from the current channel, a VFO scan steps the VFO by its
// step between lo_hz and hi_hz (both 0: the band the VFO is in), dual watch alternates between the current channel
// and zone position pos. Stop stays where the scan is, cancel goes back to where it started. Nuisance leaves the
// current stop out until the scan ends and moves on. Any channel, zone or VFO change, PTT, or hotspot mode stops it
// (dual watch: PTT doesn't, and a change starts from the selected channel; radio_req_dual)
typedef enum { RSCAN_STOP, RSCAN_ZONE, RSCAN_VFO, RSCAN_DUAL, RSCAN_CANCEL, RSCAN_NUISANCE, RSCAN_TOGGLE } radio_scan_cmd_t;
void radio_req_scan(uint8_t cmd);		// radio_scan_cmd_t; TOGGLE: stop if scanning, else zone (or VFO in the VFO)
void radio_req_scan_vfo(uint32_t lo_hz, uint32_t hi_hz);
void radio_req_scan_dual(uint16_t pos);		// the current channel (selected) and zone position pos
// Dual watch (CONFIG_DUAL_WATCH; docs/spec/radio-features.md 2.1): the receiver alternates between two channels, each
// a channel of any zone or the VFO (its own settings). ref[sel] is the selected one: PTT retunes to it, transmits,
// and the watch goes on after. Asking again with the same pair only changes the selection. NULL: stop on the
// selected channel
typedef struct {
	bool vfo;
	uint16_t zone, pos;	// a channel: zone index and position in it; a VFO: pos is which, 0 or 1
} radio_dual_ref_t;
void radio_req_dual(const radio_dual_ref_t ref[2], uint8_t sel);
// VFO `index`'s own settings (0 or 1), whether or not the radio is in it now (CONFIG_DUAL_WATCH). False: no frequency
// yet
bool radio_get_vfo(uint8_t index, channel_t *out);
// Dual watch now (CONFIG_SCAN): the two channels, the selected one and the one the receiver is on. False while it is off
bool radio_get_dual(radio_dual_ref_t ref[2], uint8_t *sel, uint8_t *at);
// DMR repeater roaming (CONFIG_DMR_ROAMING; no-op otherwise; docs/spec/dmr-roaming.md 6.1): the current zone's
// roaming flag (stored), roam now (a search at once, clears a lock), lock to the current site (RAM only)
typedef enum {
	RROAM_ZONE_OFF, RROAM_ZONE_ON, RROAM_ZONE_TOGGLE, RROAM_NOW, RROAM_LOCK, RROAM_UNLOCK, RROAM_LOCK_TOGGLE,
} radio_roam_cmd_t;
void radio_req_roam(uint8_t cmd);
// A beep (radio/beep.h beep_t) at the settings' level: key beeps only when key_beep is set (CONFIG_BEEP)
void radio_req_beep(uint8_t beep);
// A vibration pattern (radio/vibe.h vibe_t) on the board's motor (CONFIG_VIBRATE; nothing on a radio without
// hal_power_t.vibrate). Whether an alert vibrates is the caller's setting
void radio_req_vibrate(uint8_t pattern);
// FM 1750 Hz tone burst while on (CONFIG_FM_TX_TONES): keys up if not transmitting, stops when off or max_ms after
// it started (0: no limit but the TX time-out), so a host that goes away can't leave the radio keyed
void radio_req_tone_burst(bool on, uint32_t max_ms);
// A DTMF key down / up (CONFIG_DTMF; radio-features.md 3.7): digit '0'-'9', '*', '#', 'A'-'D'. Ignored unless the
// view's dtmf_ready; the tone lasts at least RADIO_DTMF_MIN_MS. The input task sends the keypad's edges (any task)
void radio_req_dtmf(char digit, bool down);
// The view's dtmf_ready as last published, cheap for the input task (false without CONFIG_DTMF)
bool radio_dtmf_ready(void);
// Speaker volume steps (radio-features.md 4.2): 0 (mute) .. RADIO_VOLUME_MAX, passed to hal_radio_t.volume() as a
// percent, round(step * 100 / RADIO_VOLUME_MAX). The step count is this one constant: the stored form, the screens,
// the debug link and the boards' tables follow it. A radio without a volume knob (radio_volume_soft(): the board has
// the hook and hal_power_t.volume() reports -1) is set by the stored step; a radio whose MCU reads the knob follows
// the knob unless the debug link overrides it (radio_req_volume_override)
#define RADIO_VOLUME_MAX	16
#define RADIO_VOLUME_DEFAULT	8
#define RADIO_VOLUME_KNOB	0xFF	// the debug link's "back to the knob" (and, with no knob, back to the default)
typedef enum {
	RVOL_NONE,		// the board has no hook: the volume works in analog hardware (the MCU can't set it)
	RVOL_KNOB,		// the MCU reads the knob and passes it on
	RVOL_SOFT,		// no knob: the stored step
	RVOL_OVERRIDE,		// a knob radio with the debug link's level in force
} radio_volume_src_t;
void radio_req_volume(uint8_t step);
// The level over the knob (step 0..RADIO_VOLUME_MAX), or -1 to give it back; RAM only. Does nothing without a knob
void radio_req_volume_override(int8_t step);
bool radio_volume_soft(void);
// Whether the MCU can set the speaker level at all (the board has the hook): knob radios accept an override
bool radio_volume_settable(void);
uint8_t radio_volume_percent(uint8_t step);
// settings_t.volume (0 = default, else a stored step) to a step, and back. A byte written by the 0..10 version (1..11)
// is scaled to the new range; the new form has bit 7 set
uint8_t radio_volume_step(uint8_t stored);
uint8_t radio_volume_stored(uint8_t step);
// A beep's level when the MCU sets the speaker volume (the board has hal_radio_t.volume) and that volume doesn't
// reach the PCM path the beeps play on (volume_pcm false): the volume percent last given to the board, up to the
// beep_level setting (cap). volume_percent < 0 (none given yet): the cap
uint8_t radio_beep_level(uint8_t cap, int16_t volume_percent);

// Settings: radio_settings_default() fills the built-in defaults; set copies them in and applies them. get answers
// with the last ones set until the radio task has taken them in, so a caller that reads, changes one field and sets
// (the UI, the debug link) keeps every other caller's change, its own earlier ones included
void radio_settings_default(radio_settings_t *s);
void radio_set_settings(const radio_settings_t *s);
void radio_get_settings(radio_settings_t *s);
// view.power_off without copying the view (the housekeeping task polls it)
bool radio_power_off_wanted(void);
// Switching off: the radio core sends what has to go out first (the ARS deregistration). radio_power_off_busy() is
// true until that is done; the caller gives up after a few seconds (gotcha 7.4)
void radio_req_power_off(void);
bool radio_power_off_busy(void);
// The radio is still running after a power off (the knob holds the supply): back to normal operation
void radio_req_power_off_cancel(void);
// The backlight percent for the UI: batt_backlight_pct with the settings and the current battery level
uint8_t radio_backlight_pct(uint8_t brightness, uint32_t idle_ms);
// The radio does something the user would want to see, so the backlight comes on: a transmission they keyed, a
// refused PTT, a call that plays (DMR) or an open squelch (FM). Not the radio's own traffic: ARS, ACKs, location
// answers, roaming probes, APRS beacons, data or calls it receives but doesn't play, a broadcast program, a hotspot.
// What reaches the user as a message, a call alert or an emergency wakes it through the UI (its banner)
bool radio_view_wakes(const radio_view_t *v);

#endif
