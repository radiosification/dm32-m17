// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The list screens and the screen table (information-architecture.md 4, 6, 8): the Hub with its six destinations,
// actions on a station, Contacts, Messages, Recent, Channels, Tools, Settings grouped by intent, the talk target
// picker, information pages and the one confirmation (factory reset, P7).
#include <stddef.h>
#include <string.h>
#include "board.h"
#include "ui_nav.h"
#include "ui_svc.h"
#include "fmt.h"
#include "codeplug/codeplug.h"
#include "config.h"
#include "hal_time.h"
#include "radio/tones.h"
#include "radio/mic_gain.h"
#include "radio/ref_trim.h"
#include "radio/radio_am.h"
#include "input/keyassign.h"
#include "rfoundry_about.h"

#ifndef RFOUNDRY_GIT
#define RFOUNDRY_GIT "host"
#endif
#ifndef RFOUNDRY_RELEASE
#define RFOUNDRY_RELEASE ""
#endif
#ifndef RFOUNDRY_GIT_DATE
#define RFOUNDRY_GIT_DATE ""
#endif

extern const ui_scr_def_t ui_find_screen;
extern const ui_scr_def_t ui_aprs_screen, ui_aprs_heard_screen, ui_aprs_pos_screen;	// ui_aprs.c
extern const ui_scr_def_t ui_gps_screen;	// ui_gps.c
#if CONFIG_DUAL_WATCH
extern const ui_scr_def_t ui_dual_screen, ui_dual_pick_screen, ui_dual_zone_screen;	// ui_dual.c
#endif

static void item_num(ui_item_t *it, uint16_t n)
{
	it->num = (char)('0' + n % 10u);
}

void item_num_set(ui_item_t *it, uint16_t n)
{
	item_num(it, n);
}

// ======================= Hub =======================

enum { HUB_LAST_CALLER, HUB_CONTACTS, HUB_MESSAGES, HUB_RECENT, HUB_ZONES, HUB_CHANNELS, HUB_TOOLS, HUB_SETTINGS };

static bool hub_has_last(void)
{
	return !ui_has_action(ACT_CONTEXT);	// few-key boards: "Last caller" stands in for CONTEXT (7.5)
}

static uint16_t hub_count(void) { return hub_has_last() ? 8 : 7; }

static uint8_t hub_entry(uint16_t i)
{
	return (uint8_t)(hub_has_last() ? i : i + 1u);
}

static void hub_item(uint16_t i, ui_item_t *it)
{
	static const char *const names[] = { TRN("Last caller"), TRN("Contacts"), TRN("Messages"), TRN("Recent"),
					     TRN("Zones"), TRN("Channels"), TRN("Tools"), TRN("Settings") };
	uint8_t e = hub_entry(i);
	str_copy(it->label, sizeof it->label, tr(names[e]));
	item_num(it, e);
	sbuf_t b;
	sb_init(&b, it->value, sizeof it->value);
	if (e == HUB_MESSAGES && ui_st.unread) {
		sb_u32(&b, ui_st.unread, 1);
		sb_str(&b, TR(" new"));
		it->flags |= UI_ITEM_WARN;
	} else if (e == HUB_ZONES) {
		// The zone (or VFO), left out where it doesn't fit beside the label (an empty last alternate)
		sb_str(&b, TEXT_ALT_OPEN);
		sb_str(&b, ui_view()->vfo ? "VFO" : ui_view()->zone_name);
		sb_str(&b, TEXT_ALT_NEXT TEXT_ALT_CLOSE);
	} else if (e == HUB_TOOLS && ui_st.scanning) {
		sb_str(&b, TR("Scanning"));
	}
}

static void hub_select(uint16_t i, bool hold)
{
	static const uint8_t scr[] = { SCR_RECENT, SCR_CONTACTS, SCR_MESSAGES, SCR_RECENT, SCR_ZONES, SCR_CHANNELS,
				       SCR_TOOLS, SCR_SETTINGS };
	(void)hold;
	uint8_t e = hub_entry(i);
	if (e == HUB_LAST_CALLER) {
		if (ui_recent_count() == 0) {
			ui_toast(TR("No calls heard yet"));
			return;
		}
		ui_target_t t;
		ui_recent_target(ui_recent_get(0), &t);
		ui_open_actions(&t);
		return;
	}
	if (!CONFIG_CHANNEL_LIST && e == HUB_CHANNELS) {
		ui_toast(TR("Channels: not in this build"));	// the numbers stay the same in every build
		return;
	}
	ui_push(scr[e], 0);
}

static const ui_scr_def_t hub_screen = {
	.name = "hub", .flags = SF_LIST | SF_NUMBERED | SF_FIELD, .hint = TRN("1-7 open  Back"),
	.count = hub_count, .item = hub_item, .select = hub_select,
};

// ======================= Actions on a station or talkgroup =======================

enum { A_TALK, A_PRIVATE, A_MESSAGE, A_CHECK, A_ALERT, A_POSITION, A_SAVE_CONTACT, A_SPEAK, A_SAVE_CHANNEL,
       A_MAKE_DEFAULT };

static uint8_t act_list(uint8_t *out)
{
	uint8_t n = 0;
	if (ui_target.group) {
		out[n++] = A_TALK;
		out[n++] = A_MAKE_DEFAULT;
		out[n++] = A_MESSAGE;
		if (CONFIG_CONTACT_EDIT && !ui_target.saved && ui_target.id != 0xFFFFFFu)
			out[n++] = A_SAVE_CONTACT;
	} else {
		static const uint8_t person[] = { A_TALK, A_PRIVATE, A_MESSAGE, A_CHECK, A_ALERT, A_POSITION,
						  A_SAVE_CONTACT, A_SPEAK };
		memcpy(out, person, sizeof person);
		n = sizeof person;
	}
	if (ui_view()->temporary)
		out[n++] = A_SAVE_CHANNEL;
	return n;
}

static uint16_t act_count(void)
{
	uint8_t l[12];
	return act_list(l);
}

static bool act_target(uint16_t i, ui_target_t *t)
{
	uint8_t l[12];
	uint8_t n = act_list(l);
	*t = ui_target;
	if (i < n && l[i] == A_PRIVATE)
		return true;	// a person, privately
	if (!t->group && t->heard_tg != 0) {
		// Talk to: the talkgroup they were heard on
		t->id = t->heard_tg;
		t->group = true;
		t->name[0] = 0;
	}
	return true;
}

static uint32_t feats(void) { return ui_svc_features(); }

static void act_item(uint16_t i, ui_item_t *it)
{
	uint8_t l[12];
	uint8_t n = act_list(l);
	if (i >= n)
		return;
	item_num(it, (uint16_t)(i + 1u));
	sbuf_t b;
	sb_init(&b, it->label, sizeof it->label);
	char name[32];
	ui_target_t t;
	switch (l[i]) {
	case A_TALK:
		act_target(i, &t);
		if (t.group)
			t.name[0] = 0;	// "Talk to TG 91": the name is in the title
		ui_sb_target(name, sizeof name, &t);
		sb_str(&b, TR("Talk to "));
		sb_str(&b, name);
		if (!(feats() & UI_FEAT_TALK_TARGET))
			it->flags |= UI_ITEM_DIM;
		break;
	case A_PRIVATE:
		sb_str(&b, TR("Private call"));
		if (!(feats() & UI_FEAT_TALK_TARGET))
			it->flags |= UI_ITEM_DIM;
		break;
	case A_MESSAGE:
		sb_str(&b, TR("Message"));
		if (!(feats() & UI_FEAT_SMS))
			it->flags |= UI_ITEM_DIM;
		break;
	case A_CHECK:
		sb_str(&b, TR("Radio check"));
		if (!(feats() & UI_FEAT_SIGNALING))
			it->flags |= UI_ITEM_DIM;
		break;
	case A_ALERT:
		sb_str(&b, TR("Call alert"));
		if (!(feats() & UI_FEAT_SIGNALING))
			it->flags |= UI_ITEM_DIM;
		break;
	case A_POSITION:
		sb_str(&b, TR("Position"));
		it->flags |= UI_ITEM_DIM;
		break;
	case A_SAVE_CONTACT:
		sb_str(&b, ui_target.saved ? TR("In contacts") : TR("Save as contact"));
		if (ui_target.saved || !CONFIG_CONTACT_EDIT)
			it->flags |= UI_ITEM_DIM;
		break;
	case A_SPEAK:
		sb_str(&b, TR("Speak status"));
		it->flags |= UI_ITEM_DIM;
		break;
	case A_SAVE_CHANNEL:
		sb_str(&b, TR("Save to channel"));
		if (!(feats() & UI_FEAT_SAVE_CHANNEL))
			it->flags |= UI_ITEM_DIM;
		break;
	case A_MAKE_DEFAULT:
		sb_str(&b, TR("Make channel default"));
		if (!(feats() & UI_FEAT_TALK_TARGET))
			it->flags |= UI_ITEM_DIM;
		break;
	default:
		break;
	}
}

static void act_select(uint16_t i, bool hold)
{
	static char msg[40];
	uint8_t l[12];
	uint8_t n = act_list(l);
	if (i >= n)
		return;
	ui_target_t t;
	char name[24];
	ui_sb_target(name, sizeof name, &ui_target);
	sbuf_t b;
	sb_init(&b, msg, sizeof msg);
	switch (l[i]) {
	case A_TALK:
		act_target(i, &t);
		ui_talk(&t, hold);
		break;
	case A_PRIVATE:
		t = ui_target;
		t.group = false;
		ui_talk(&t, false);
		break;
	case A_MAKE_DEFAULT:
		ui_talk(&ui_target, true);
		break;
	case A_MESSAGE:
		if (feats() & UI_FEAT_SMS)
			ui_push(SCR_COMPOSE, 0);
		else
			ui_toast(TR("Messages: not in this build"));
		break;
	case A_CHECK:
		if (ui_svc_radio_check(ui_target.id)) {
			sb_str(&b, TR("Checking "));
			sb_str(&b, name);
			sb_str(&b, "...");
			ui_go_home();
			ui_toast(msg);
		} else {
			ui_toast(TR("Radio check: not in this build"));
		}
		break;
	case A_ALERT:
		if (ui_svc_call_alert(ui_target.id)) {
			sb_str(&b, TR("Call alert to "));
			sb_str(&b, name);
			ui_go_home();
			ui_toast(msg);
		} else {
			ui_toast(TR("Call alert: not in this build"));
		}
		break;
	case A_POSITION:
		ui_toast(TR("Position requests: not in this build"));
		break;
	case A_SAVE_CONTACT:
		ui_toast(ui_target.saved ? TR("Already in your contacts") : TR("Add contacts in qdmr"));
		break;
	case A_SPEAK:
		ui_toast(TR("Voice prompts: not in this build"));
		break;
	case A_SAVE_CHANNEL:
		if (ui_try_save_channel())
			ui_go_home();
		break;
	default:
		break;
	}
}

// The station or talkgroup by name ("Joe Smith", "Main Ops"), else by number; the rows give the numbers
static const char *act_title(void)
{
	static char t[40];
	if (ui_target.group && ui_target.name[0] != '\0') {
		strcpy(t, ui_target.name);
		return t;
	}
	ui_sb_target(t, sizeof t, &ui_target);
	return t;
}

static const ui_scr_def_t actions_screen = {
	.name = "actions", .flags = SF_LIST | SF_NUMBERED | SF_FIELD, .hint = TRN("1-9 pick  PTT talk  Back"),
	.count = act_count, .item = act_item, .select = act_select, .target = act_target, .title = act_title,
};

// The message view and text entry never draw together; share their scratch space on RAM-limited radios.
static char text_buf[UI_MSG_TEXT_MAX + 48];

// ======================= Message composer =======================

// Sent in the language in use (the text goes out as UTF-16, Latin-1 included)
static const char *const canned[] = { TRN("OK"), TRN("On my way"), TRN("Call me"), TRN("Can't talk now") };
#define CANNED_N	(sizeof canned / sizeof canned[0])

static uint16_t compose_count(void) { return CANNED_N + 1; }

static void compose_item(uint16_t i, ui_item_t *it)
{
	if (i < CANNED_N) {
		sbuf_t b;
		sb_init(&b, it->label, sizeof it->label);
		sb_chr(&b, '"');
		sb_str(&b, tr(canned[i]));
		sb_chr(&b, '"');
	} else {
		str_copy(it->label, sizeof it->label, TR("Write..."));
		if (!CONFIG_DMR_SMS)
			it->flags |= UI_ITEM_DIM;
	}
}

static void send_text(const char *text)
{
	static char msg[40];
	char name[24];
	ui_sb_target(name, sizeof name, &ui_target);
	if (!ui_svc_msg_send(ui_target.id, ui_target.group, text)) {
		ui_toast(TR("Couldn't send: try again"));
		return;
	}
	sbuf_t b;
	sb_init(&b, msg, sizeof msg);
	sb_str(&b, TR("Sending to "));
	sb_str(&b, name);
	ui_go_home();
	ui_toast(msg);
}

static void compose_select(uint16_t i, bool hold)
{
	(void)hold;
	if (i < CANNED_N)
		send_text(tr(canned[i]));
	else {
		ui_toast(TR("Typing: not in this build"));
	}
}

static const char *compose_title(void)
{
	static char t[32];
	ui_sb_target(t, sizeof t, &ui_target);
	return t;
}

static const ui_scr_def_t compose_screen = {
	.name = "compose", .flags = SF_LIST | SF_FIELD, .prefix = TRN("To"), .hint = TRN("OK send  Back"),
	.count = compose_count, .item = compose_item, .select = compose_select, .title = compose_title,
};

// ======================= Contacts (type to find) =======================

static uint16_t none_count(void) { return 0; }
static void none_item(uint16_t i, ui_item_t *it) { (void)i; (void)it; }
static const ui_scr_def_t contacts_screen = {
	.name = "contacts", .flags = SF_LIST | SF_FIELD, .prefix = TRN("Contacts"), .hint = TRN("Back"),
	.empty = TRN("Contacts: not in this build"), .count = none_count, .item = none_item,
};

// ======================= Messages =======================

static uint16_t msgs_count(void) { return ui_svc_msg_count(); }

static void msgs_item(uint16_t i, ui_item_t *it)
{
	ui_msg_t m;
	if (!ui_svc_msg_get(i, &m))
		return;
	sbuf_t b;
	sb_init(&b, it->label, sizeof it->label);
	if (m.sent)
		sb_str(&b, GLYPH_ARROW);
	if (m.name[0])
		sb_str(&b, m.name);
	else
		sb_u32(&b, m.src, 1);
	sb_str(&b, ": ");
	sb_str(&b, m.text);
	ui_sb_age(it->value, sizeof it->value, m.age_ms);
	it->flags |= UI_ITEM_PREVIEW;	// the start of the message: cut at its end
	if (m.unread)
		it->flags |= UI_ITEM_WARN;
}

static void msgs_select(uint16_t i, bool hold)
{
	(void)hold;
	ui_push(SCR_MESSAGE, i);
}

static void msg_target_of(uint16_t i, ui_target_t *t)
{
	ui_msg_t m;
	memset(t, 0, sizeof *t);
	if (ui_svc_msg_get(i, &m)) {
		t->id = m.src;
		strcpy(t->name, m.name);
		t->saved = m.name[0] != 0;
	}
}

static void msgs_context(uint16_t i)
{
	ui_target_t t;
	msg_target_of(i, &t);
	ui_open_actions(&t);
}

static const ui_scr_def_t messages_screen = {
	.name = "messages", .flags = SF_LIST | SF_FIELD, .prefix = TRN("Messages"), .hint = TRN("OK open  # reply  Back"),
	.empty = TRN("No messages yet"),
	.count = msgs_count, .item = msgs_item, .select = msgs_select, .context = msgs_context,
};

// One message: the text, then canned replies first, call, delete

enum { MV_REPLY_OK, MV_REPLY_WAY, MV_CALL, MV_DELETE, MV_COUNT };

static void msg_enter(void)
{
	ui_svc_msg_read(ui_frame()->arg);
}

static uint16_t msg_count(void) { return MV_COUNT; }

static void msg_item(uint16_t i, ui_item_t *it)
{
	static const char *const names[] = { TRN("Reply \"OK\""), TRN("Reply \"On my way\""), TRN("Call"),
					     TRN("Delete") };
	str_copy(it->label, sizeof it->label, tr(names[i]));
	if (i <= MV_REPLY_WAY && !(feats() & UI_FEAT_SMS))
		it->flags |= UI_ITEM_DIM;
}

static void msg_select(uint16_t i, bool hold)
{
	(void)hold;
	msg_target_of(ui_frame()->arg, &ui_target);
	switch (i) {
	case MV_REPLY_OK:
	case MV_REPLY_WAY:
		send_text(tr(i == MV_REPLY_OK ? canned[0] : canned[1]));
		break;
	case MV_CALL:
		ui_talk(&ui_target, false);
		break;
	case MV_DELETE:
		if (ui_svc_msg_delete(ui_frame()->arg)) {
			ui_pop();
			ui_toast(TR("Message deleted"));
		} else {
			ui_toast(TR("Couldn't delete: try again"));
		}
		break;
	default:
		break;
	}
}

static bool msg_target(uint16_t i, ui_target_t *t)
{
	(void)i;
	msg_target_of(ui_frame()->arg, t);
	return t->id != 0;
}

static const char *msg_title(void)
{
	static char t[24];
	sbuf_t b;
	sb_init(&b, t, sizeof t);
	sb_u32(&b, ui_frame()->arg + 1u, 1);
	sb_str(&b, TR(" of "));
	sb_u32(&b, ui_svc_msg_count(), 1);
	return t;
}

static const char *msg_body(void)
{
	ui_msg_t m;
	if (!ui_svc_msg_get(ui_frame()->arg, &m))
		return TR("Message deleted");
	// "From Joe Smith, 2m ago" (the ID where there is no name; # gives the station's actions), then the text
	sbuf_t b;
	sb_init(&b, text_buf, sizeof text_buf);
	sb_str(&b, m.sent ? TR("To ") : TR("From "));
	if (m.name[0])
		sb_str(&b, m.name);
	else
		sb_u32(&b, m.src, 1);
	sb_str(&b, ", ");
	sb_age_long(&b, m.age_ms);
	sb_chr(&b, '\n');
	sb_str(&b, m.text);
	return text_buf;
}

static const ui_scr_def_t message_screen = {
	.name = "message", .flags = SF_LIST | SF_FIELD, .prefix = TRN("Message"), .hint = TRN("OK choose  # more  Back"),
	.count = msg_count, .item = msg_item, .select = msg_select, .context = msgs_context, .target = msg_target,
	.title = msg_title, .body = msg_body, .enter = msg_enter,
};

// ======================= Recent =======================

static uint16_t recent_count(void) { return ui_recent_count(); }

static bool recent_target(uint16_t i, ui_target_t *t)
{
	const ui_recent_t *r = ui_recent_get((uint8_t)i);
	if (r == NULL)
		return false;
	ui_recent_target(r, t);
	if (!t->group && t->heard_tg) {
		// PTT on a station heard on a talkgroup: that talkgroup (the same as Talk to in its actions)
		t->id = t->heard_tg;
		t->group = true;
		t->name[0] = 0;
	}
	return true;
}

static void recent_item(uint16_t i, ui_item_t *it)
{
	const ui_recent_t *r = ui_recent_get((uint8_t)i);
	if (r == NULL)
		return;
	sbuf_t b;
	sb_init(&b, it->label, sizeof it->label);
	sbuf_t vb;
	sb_init(&vb, it->value, sizeof it->value);
	if (r->out) {
		sb_str(&b, GLYPH_ARROW);
		sb_str(&b, r->group ? "TG " : "");
		sb_u32(&b, r->dst, 1);
		if (r->name[0]) {
			sb_chr(&b, ' ');
			sb_str(&b, r->name);
		}
	} else {
		if (r->name[0])
			sb_str(&b, r->name);
		else
			sb_u32(&b, r->src, 1);
		if (r->group) {
			sb_str(&vb, "TG ");
			sb_u32(&vb, r->dst, 1);
		} else {
			sb_str(&vb, r->to_us ? TR("To you") : TR("Private"));
		}
		sb_chr(&vb, ' ');
		if (r->gps_range) {	// the caller's position: how far
			sb_u32(&vb, (r->gps_dist_m + 500u) / 1000u, 1);
			sb_str(&vb, "km ");
		}
	}
	sb_age(&vb, ui_st.now_ms - r->ms);
	if (r->to_us)
		it->flags |= UI_ITEM_WARN;
}

static void recent_select(uint16_t i, bool hold)
{
	ui_target_t t;
	if (recent_target(i, &t))
		ui_talk(&t, hold);
}

static void recent_context(uint16_t i)
{
	const ui_recent_t *r = ui_recent_get((uint8_t)i);
	ui_target_t t;
	if (r != NULL) {
		ui_recent_target(r, &t);
		ui_open_actions(&t);
	}
}

static const ui_scr_def_t recent_screen = {
	.name = "recent", .flags = SF_LIST | SF_FIELD, .prefix = TRN("Recent"), .hint = TRN("PTT talk  # more  Back"),
	.empty = TRN("No calls heard yet"),
	.count = recent_count, .item = recent_item, .select = recent_select, .context = recent_context,
	.target = recent_target,
};

// ======================= Channels: VFO, the zones; a zone's channels =======================

#define CH_VFO	((CONFIG_VFO_FM || CONFIG_VFO_DMR) ? 1u : 0u)	// raw row 0 is the VFO when it is built

static bool chans_is_vfo(uint16_t raw) { return (CONFIG_VFO_FM || CONFIG_VFO_DMR) && raw == 0; }
// With the zone editor the last row makes a new zone (named first, then empty: its list adds channels)
static bool chans_is_new(uint16_t raw) { return CONFIG_ZONE_EDIT && raw == CH_VFO + codeplug_zone_count(); }
static uint16_t chans_raw_count(void) { return (uint16_t)(CH_VFO + codeplug_zone_count() + (CONFIG_ZONE_EDIT ? 1u : 0u)); }
#if CONFIG_ZONE_EDIT
static void zones_new(void);
#endif

static bool chans_raw_label(uint16_t raw, char *buf, uint8_t len)
{
	static zone_t z;
	if (chans_is_vfo(raw)) {
		strncpy(buf, "VFO", len);
		return true;
	}
	if (chans_is_new(raw)) {
		str_copy(buf, len, TR("Create New Zone"));
		return true;
	}
	if (!codeplug_zone((uint16_t)(raw - CH_VFO), &z))
		return false;
	strncpy(buf, z.name, len - 1);
	buf[len - 1] = 0;
	return true;
}

static void chans_enter(void)
{
	// Focus the current zone (task-flows.md 2)
	const radio_view_t *v = ui_view();
	uint16_t want = v->vfo ? 0 : (uint16_t)(CH_VFO + v->zone_index);
	for (uint16_t i = 0; i < ui_filter_count(); i++)
		if (ui_filter_raw(i) == want)
			ui_frame()->focus = i;
}

static void chans_item(uint16_t i, ui_item_t *it)
{
	static zone_t z;
	const radio_view_t *v = ui_view();
	uint16_t raw = ui_filter_raw(i);
	sbuf_t vb;
	sb_init(&vb, it->value, sizeof it->value);
	if (chans_is_vfo(raw)) {
		strcpy(it->label, "VFO");
		if (v->vfo)
			sb_str(&vb, TR("On"));
		return;
	}
	if (chans_is_new(raw)) {
		str_copy(it->label, sizeof it->label, TR("Create New Zone"));
		return;
	}
	uint16_t zi = (uint16_t)(raw - CH_VFO);
	if (!codeplug_zone(zi, &z))
		return;
	str_copy(it->label, sizeof it->label, z.name);
	// The zone's name whole; beside it "Here" for the zone in use and the channel count, as much as fits:
	// "Here, 16 channels", "Here, 16 ch", "Here" (or "16 channels", "16 ch", nothing)
	bool here = !v->vfo && zi == v->zone_index;
	sb_str(&vb, TEXT_ALT_OPEN);
	for (int form = 0; form < 3; form++) {
		if (form)
			sb_str(&vb, TEXT_ALT_NEXT);
		if (here)
			sb_str(&vb, form < 2 ? TR("Here, ") : TR("Here"));
		if (form < 2) {
			sb_u32(&vb, z.count, 1);
			sb_str(&vb, form == 0 ? (z.count == 1 ? TR(" channel") : TR(" channels")) : TR(" ch"));
		}
	}
	sb_str(&vb, TEXT_ALT_CLOSE);
}

static void chans_context(uint16_t i);

static void chans_select(uint16_t i, bool hold)
{
	static char msg[CODEPLUG_NAME_LEN + 8];
	static zone_t z;
	if (hold && !ui_has_action(ACT_CONTEXT)) {
		chans_context(i);	// few-key boards: long OK opens what CONTEXT would (information-architecture 7.5)
		return;
	}
	const radio_view_t *v = ui_view();
	uint16_t raw = ui_filter_raw(i);
#if CONFIG_ZONE_EDIT
	if (chans_is_new(raw)) {
		zones_new();
		return;
	}
#endif
	if (chans_is_vfo(raw)) {
		bool was = v->vfo;	// the view may change at once (the host simulator)
		radio_req_vfo(!was);
		ui_go_home();
		ui_toast(was ? TR("Back to channels") : TR("VFO: type a frequency"));
		return;
	}
	uint16_t zi = (uint16_t)(raw - CH_VFO);
	if (v->vfo)
		radio_req_vfo(false);
	radio_req_zone(zi);
	ui_go_home();
	if (codeplug_zone(zi, &z)) {
		sbuf_t b;
		sb_init(&b, msg, sizeof msg);
		sb_str(&b, TR("Zone: "));
		sb_str(&b, z.name);
		ui_toast(msg);
	}
}

static void chans_context(uint16_t i)
{
	uint16_t raw = ui_filter_raw(i);
	if (chans_is_vfo(raw)) {
		ui_toast(TR("The VFO has no channel list"));
		return;
	}
#if CONFIG_ZONE_EDIT
	if (chans_is_new(raw)) {
		zones_new();
		return;
	}
#endif
	ui_push(SCR_ZONE, (uint16_t)(raw - CH_VFO));
}

static const ui_scr_def_t channels_screen = {
	.name = "zones", .flags = SF_LIST | SF_FILTER | SF_FIELD, .prefix = TRN("Zones"),
	.hint = TRN(TEXT_ALT_OPEN "OK go  # channels  0-9 find" TEXT_ALT_NEXT "OK go  # channels" TEXT_ALT_CLOSE),
	.empty = TRN("No zones"),
	.count = ui_filter_count, .item = chans_item, .select = chans_select, .context = chans_context,
	.enter = chans_enter, .raw_count = chans_raw_count, .raw_label = chans_raw_label,
};

// A channel row's value: its RX frequency and what it talks to, the talkgroup on DMR ("TG3113", "ID3101234" for a
// private contact) or "FM". The whole name comes first; where both don't fit beside it, the frequency and the bare
// number, then the talkgroup alone (text alternates)
static void sb_channel_row(sbuf_t *b, const channel_t *ch)
{
	char what[16];
	uint8_t prefix = 0;	// "TG" / "ID": left out in the middle form
	sbuf_t wb;
	sb_init(&wb, what, sizeof what);
	if (codeplug_mode_digital_voice(ch->mode)) {
		// P25 and NXDN: the talkgroup, as DMR ("TG100")
		sb_str(&wb, ch->mode == RADIO_MODE_M17 ? "GR" : "TG");
		prefix = 2;
		sb_u32(&wb, ch->talkgroup, 1);
	} else {
		sb_str(&wb, ch->mode == RADIO_MODE_DMR ? "DMR" : radio_am_channel(ch) ? "AM" : "FM");
	}
	sb_str(b, TEXT_ALT_OPEN);
	sb_freq(b, ch->rx_hz);
	sb_chr(b, ' ');
	sb_str(b, what);
	if (prefix) {
		sb_str(b, TEXT_ALT_NEXT);
		sb_freq(b, ch->rx_hz);
		sb_chr(b, ' ');
		sb_str(b, what + prefix);
	}
	sb_str(b, TEXT_ALT_NEXT);
	sb_str(b, what);
	sb_str(b, TEXT_ALT_CLOSE);
}

static zone_t zone_cache;
static uint16_t zone_cached = 0xFFFF;

// With the zone editor (ui.md 2.2) the list's raw rows are "Add channel", the channels, "Zone options"
#define ZE_ADD		(CONFIG_ZONE_EDIT ? 1u : 0u)
#if CONFIG_ZONE_EDIT
#if !CONFIG_CHANNEL_EDIT
#error "CONFIG_ZONE_EDIT needs CONFIG_CHANNEL_EDIT"
#endif
static uint16_t zone_focus_num;		// the channel the zone's list focuses when it shows again (ui_menus_resume)
static uint16_t ze_zone;		// the zone the editor's screens work on (the zone list's index)
static zone_op_t ze_op;			// the change handed to the storage job (ui_svc_zone_edit)
static char ze_msg[CODEPLUG_NAME_LEN + 24];	// the editor's toasts
static char ze_name[CODEPLUG_NAME_LEN + 1];	// a zone name being typed (Rename, Create New Zone)

static zone_op_t *ze_op_new(uint8_t kind, uint16_t pos)
{
	memset(&ze_op, 0, sizeof ze_op);
	ze_op.kind = kind;
	ze_op.zone = ze_zone;
	ze_op.pos = pos;
	return &ze_op;
}

static bool ze_post(void)
{
	if (ui_svc_zone_edit(&ze_op))
		return true;
	ui_toast(TR("Couldn't save now: try again"));
	return false;
}

// "<text><name>" in ze_msg, as a toast
static void ze_toast(const char *text, const char *name)
{
	sbuf_t b;
	sb_init(&b, ze_msg, sizeof ze_msg);
	sb_str(&b, text);
	sb_str(&b, name);
	ui_toast(ze_msg);
}
#endif

static const zone_t *zone_of_frame(void)
{
	uint16_t zi = ui_frame()->arg;
	if (zi != zone_cached) {
		if (!codeplug_zone(zi, &zone_cache))
			zone_cache.count = 0;
		zone_cached = zi;
	}
	return &zone_cache;
}

static uint16_t zone_raw_count(void)
{
	zone_cached = 0xFFFF;	// a fresh read when the list opens or the filter changes
	return (uint16_t)(zone_of_frame()->count + 2u * ZE_ADD);
}

#if CONFIG_ZONE_EDIT
// The editor's rows: their label, NULL for a channel's row
static const char *zone_edit_row(uint16_t raw)
{
	if (raw == 0)
		return TR("Add channel");
	return raw > zone_of_frame()->count ? TR("Zone options") : NULL;
}
#endif

static bool zone_raw_label(uint16_t raw, char *buf, uint8_t len)
{
	static channel_t ch;
	const zone_t *z = zone_of_frame();
	sbuf_t b;
	sb_init(&b, buf, len);
#if CONFIG_ZONE_EDIT
	if (zone_edit_row(raw) != NULL) {
		sb_str(&b, zone_edit_row(raw));
		return true;
	}
#endif
	uint16_t pos = (uint16_t)(raw - ZE_ADD);
	if (pos >= z->count || !codeplug_channel(z->channels[pos], &ch))
		return false;
	sb_u32(&b, pos + 1u, 1);
	sb_chr(&b, ' ');
	sb_str(&b, ch.name);
	return true;
}

// Focus the row of channel number num (0: the channel in use, when this is the zone in use)
static void zone_focus(uint16_t num)
{
	const radio_view_t *v = ui_view();
	const zone_t *z = zone_of_frame();
	for (uint16_t i = 0; i < ui_filter_count(); i++) {
		uint16_t pos = (uint16_t)(ui_filter_raw(i) - ZE_ADD);
		if (pos >= z->count)
			continue;
		if (num != 0 ? z->channels[pos] == num : !v->vfo && v->zone_index == ui_frame()->arg && pos == v->zone_pos)
			ui_frame()->focus = i;
	}
}

static void zone_enter(void)
{
#if CONFIG_ZONE_EDIT
	zone_focus_num = 0;
#endif
	zone_focus(0);
}

#if CONFIG_ZONE_EDIT
// The list shows again (a menu above it closed) or the codeplug changed under it: the rows are read again
// (ui_filter_rebuild), and the focus follows the channel just moved or added
static bool zones_new_pending;	// Zones focuses the zone named ze_name once its write has landed

void ui_menus_resume(void)
{
	if (ui_frame()->scr == SCR_ZONE && zone_focus_num != 0)
		zone_focus(zone_focus_num);
	if (zones_new_pending && !ui_svc_zone_pending()) {
		// A new zone takes the lowest free record, which can be ahead of others: found by its name. Once written
		// the flag goes, whatever screen is up, so a later visit to Zones isn't moved
		char name[CODEPLUG_NAME_LEN + 1];
		for (uint16_t i = 0; ui_frame()->scr == SCR_ZONES && i < ui_filter_count(); i++) {
			uint16_t raw = ui_filter_raw(i);
			if (!chans_is_vfo(raw) && !chans_is_new(raw) && chans_raw_label(raw, name, sizeof name) &&
			    strcmp(name, ze_name) == 0)
				ui_frame()->focus = i;
		}
		zones_new_pending = false;
	}
}
#endif


static void zone_item(uint16_t i, ui_item_t *it)
{
	static channel_t ch;
	const zone_t *z = zone_of_frame();
	uint16_t raw = ui_filter_raw(i);
#if CONFIG_ZONE_EDIT
	if (zone_edit_row(raw) != NULL) {
		str_copy(it->label, sizeof it->label, zone_edit_row(raw));
		return;
	}
#endif
	raw = (uint16_t)(raw - ZE_ADD);		// the position in the zone
	if (raw >= z->count || !codeplug_channel(z->channels[raw], &ch))
		return;
	sbuf_t b;
	sb_init(&b, it->label, sizeof it->label);
	sb_u32(&b, raw + 1u, 2);
	sb_chr(&b, ' ');
	sb_str(&b, ch.name);
	sb_init(&b, it->value, sizeof it->value);
	sb_channel_row(&b, &ch);
	const radio_view_t *v = ui_view();
	if (!v->vfo && v->zone_index == ui_frame()->arg && v->zone_pos == raw)
		it->flags |= UI_ITEM_TEMP;	// the channel we are on
}

#if CONFIG_ZONE_EDIT
static void zone_context(uint16_t i)
{
	uint16_t raw = ui_filter_raw(i);
	if (zone_edit_row(raw) != NULL)
		return;
	ze_zone = ui_frame()->arg;
	ui_push(SCR_ZCHANNEL, (uint16_t)(raw - ZE_ADD));
}
#endif

static void zone_select(uint16_t i, bool hold)
{
	const radio_view_t *v = ui_view();
	uint16_t zi = ui_frame()->arg;
	uint16_t raw = ui_filter_raw(i);
#if CONFIG_ZONE_EDIT
	if (zone_edit_row(raw) != NULL) {
		ze_zone = zi;
		ui_push(raw == 0 ? SCR_ZPICK : SCR_ZONEEDIT, zi);
		return;
	}
	if (hold && !ui_has_action(ACT_CONTEXT)) {
		zone_context(i);	// few-key boards: long OK opens what CONTEXT would (information-architecture 7.5)
		return;
	}
#endif
	(void)hold;
	if (v->vfo)
		radio_req_vfo(false);
	if (v->zone_index != zi)
		radio_req_zone(zi);
	radio_req_zone_pos((uint16_t)(raw - ZE_ADD));
	ui_go_home();
}

static const char *zone_title(void)
{
	return zone_of_frame()->name;
}



#if CONFIG_ZONE_EDIT
#define ZONE_HINT	TRN(TEXT_ALT_OPEN "OK go  # edit  0-9 find" TEXT_ALT_NEXT "OK go  # edit" TEXT_ALT_CLOSE)
#define ZONE_CONTEXT	zone_context
#else
#define ZONE_HINT	TRN("OK go  0-9 find  Back")
#define ZONE_CONTEXT	NULL
#endif

static const ui_scr_def_t zone_screen = {
	.name = "zone", .flags = SF_LIST | SF_FILTER | SF_FIELD, .prefix = NULL,
	.hint = ZONE_HINT, .empty = TRN("Empty zone"),
	.count = ui_filter_count, .item = zone_item, .select = zone_select, .context = ZONE_CONTEXT,
	.title = zone_title, .enter = zone_enter, .raw_count = zone_raw_count, .raw_label = zone_raw_label,
};

#if CONFIG_ZONE_WRITE
// Zone indexes moved (zone_edit.h): the stack's zone screens follow them. Zones keeps its focus on the same zone, a
// zone's list and the editor's screens on their zone; a list whose zone was deleted closes, with what is above it.
// Returns the new top of the stack
uint8_t ui_menus_zones_moved(ui_frame_t *stack, uint8_t sp, uint32_t from, uint32_t to)
{
	uint16_t out;
#if CONFIG_ZONE_EDIT
	bool ze_ok = zone_edit_map(from, to, ze_zone, &out) == ZONE_MAP_KEPT;
	if (ze_ok)
		ze_zone = out;
#endif
	for (uint8_t i = 1; i <= sp; i++) {
		ui_frame_t *f = &stack[i];
		if (f->scr == SCR_ZONES && !chans_is_vfo(f->focus)) {
			if (zone_edit_map(from, to, (uint16_t)(f->focus - CH_VFO), &out) != ZONE_MAP_LOST)
				f->focus = (uint16_t)(out + CH_VFO);	// a deleted one: the next zone, clamped by the list
		} else if (f->scr == SCR_ZONE) {
			if (zone_edit_map(from, to, f->arg, &out) != ZONE_MAP_KEPT)
				return (uint8_t)(i - 1u);
			f->arg = out;
		}
#if CONFIG_ZONE_EDIT
		else if (f->scr == SCR_ZCHANNEL || f->scr == SCR_ZPICK || f->scr == SCR_ZONEEDIT || f->scr == SCR_ZONEDEL) {
			if (!ze_ok)
				return (uint8_t)(i - 1u);
		}
#endif
	}
	return sp;
}
#endif

#if CONFIG_CHANNEL_LIST
// ======================= Channels: every codeplug channel by number (hub 5) =======================
// The raw row is the channel number - 1 (with CONFIG_CHANNEL_EDIT, raw 0 is "New from current" and the raw
// row is the channel number); empty slots have no label, so the filter leaves them out
#define CHLIST_NEW	(CONFIG_CHANNEL_EDIT ? 1u : 0u)
#define CHLIST_NUM(raw)	((uint16_t)((raw) + 1u - CHLIST_NEW))
#if CONFIG_CHANNEL_EDIT
static bool ced_from_current;		// the next push of the channel opens a copy of what the radio uses now
#endif
#if CONFIG_ZONE_EDIT
static uint16_t ced_copy_of;		// the next push of the channel opens a copy of this one (Copy here, ui.md 2.2)
static bool ced_into;			// saving the copy puts it in zone ze_zone at ced_into_pos
static uint16_t ced_into_pos;
#endif

// The channel the radio is on, 0 in the VFO or when its zone can't be read
static uint16_t chlist_current(void)
{
	static zone_t z;
	const radio_view_t *v = ui_view();
	if (v->vfo || !codeplug_zone(v->zone_index, &z) || v->zone_pos >= z.count)
		return 0;
	return z.channels[v->zone_pos];
}

static uint16_t chlist_raw_count(void)
{
	return (uint16_t)(codeplug_channel_count() + CHLIST_NEW);
}

static bool chlist_raw_label(uint16_t raw, char *buf, uint8_t len)
{
	static channel_t ch;
	sbuf_t b;
	sb_init(&b, buf, len);
	if (CHLIST_NEW && raw == 0) {
		sb_str(&b, TR("New from current"));
		return true;
	}
	if (!codeplug_channel(CHLIST_NUM(raw), &ch))
		return false;
	sb_u32(&b, CHLIST_NUM(raw), 1);
	sb_chr(&b, ' ');
	sb_str(&b, ch.name);
	return true;
}

static void chlist_enter(void)
{
	uint16_t cur = chlist_current();
	for (uint16_t i = 0; cur != 0 && i < ui_filter_count(); i++)
		if (CHLIST_NUM(ui_filter_raw(i)) == cur)
			ui_frame()->focus = i;
}

static void chlist_item(uint16_t i, ui_item_t *it)
{
	static channel_t ch;
	if (CHLIST_NEW && ui_filter_raw(i) == 0) {
		str_copy(it->label, sizeof it->label, TR("New from current"));
		return;
	}
	uint16_t num = CHLIST_NUM(ui_filter_raw(i));
	if (!codeplug_channel(num, &ch))
		return;
	sbuf_t b;
	sb_init(&b, it->label, sizeof it->label);
	sb_u32(&b, num, 1);
	sb_chr(&b, ' ');
	sb_str(&b, ch.name);
	sb_init(&b, it->value, sizeof it->value);
	sb_channel_row(&b, &ch);
	if (num == chlist_current())
		it->flags |= UI_ITEM_TEMP;	// the channel we are on
}

// Go to channel num: in the zone in use if it holds it, else the first zone that does. false: no zone holds it
static bool chlist_tune(uint16_t num)
{
	static zone_t z;
	const radio_view_t *v = ui_view();
	uint16_t zones = codeplug_zone_count();
	for (uint16_t k = 0; k <= zones; k++) {
		// k == 0 tries the zone in use first, then every zone in order
		uint16_t zi = k == 0 ? v->zone_index : (uint16_t)(k - 1u);
		if ((k == 0 && v->vfo) || (k > 0 && zi == v->zone_index && !v->vfo) || !codeplug_zone(zi, &z))
			continue;
		for (uint16_t pos = 0; pos < z.count; pos++) {
			if (z.channels[pos] != num)
				continue;
			if (v->vfo)
				radio_req_vfo(false);
			if (v->zone_index != zi)
				radio_req_zone(zi);
			radio_req_zone_pos(pos);
			ui_go_home();
			return true;
		}
	}
	ui_toast(TR("Not in any zone: add it to one to use it"));
	return false;
}

static void chlist_select(uint16_t i, bool hold)
{
	(void)hold;
#if CONFIG_CHANNEL_EDIT
	if (ui_filter_raw(i) == 0) {
		uint16_t num = codeplug_free_channel();
		if (num == 0) {
			ui_toast(TR("No free channel"));
			return;
		}
		ced_from_current = true;
		ui_push(SCR_CHANNEL, num);
		return;
	}
#endif
	ui_push(SCR_CHANNEL, CHLIST_NUM(ui_filter_raw(i)));
}

static void chlist_context(uint16_t i)
{
	if (CHLIST_NEW && ui_filter_raw(i) == 0)
		return;
	chlist_tune(CHLIST_NUM(ui_filter_raw(i)));
}

static const ui_scr_def_t chlist_screen = {
	.name = "channels", .flags = SF_LIST | SF_FILTER | SF_FIELD, .prefix = TRN("Channels"),
	.hint = TRN(TEXT_ALT_OPEN "OK details  # go  0-9 find" TEXT_ALT_NEXT "OK details  # go" TEXT_ALT_CLOSE),
	.empty = TRN("No channels"),
	.count = ui_filter_count, .item = chlist_item, .select = chlist_select, .context = chlist_context,
	.enter = chlist_enter, .raw_count = chlist_raw_count, .raw_label = chlist_raw_label,
};

// ======================= One channel's settings, edited in place (ui.md 2.1) =======================
// Without CONFIG_CHANNEL_EDIT the rows are read only and OK goes to the channel

enum { CE_NAME, CE_NUMBER, CE_MODE, CE_RX, CE_TX, CE_POWER, CE_RX_ONLY, CE_BW, CE_RX_TONE, CE_TX_TONE, CE_VOX,
       CE_NAC, CE_TG, CE_CC, CE_TS, CE_CONTACT, CE_LIST, CE_ID, CE_DMO, CE_BEEP, CE_ALIAS1, CE_ALIAS2, CE_TOT, CE_SCAN, CE_ECO,
       CE_ZONES, CE_GO, CE_SHARE, CE_SAVE, CE_SAVE_NEW, CE_DELETE, CE_COUNT, CE_NONE = 0xFF };

static channel_t ced_ch;		// what the screen shows (the working copy when editing)
static uint16_t ced_num;		// its number, 0 = no such channel
static uint8_t ced_rows[CE_COUNT];
static uint8_t ced_n;

#if CONFIG_CHANNEL_EDIT
#define CE_TOT_MAX	495
#define CE_TOT_STEP	15
#define CE_ID_MAX	((int32_t)CODEPLUG_DMR_ID_MAX)
#define CE_DIGITS	UI_ENTRY_MAX	// a frequency, or an ID (8 digits too)

static channel_t ced_orig;		// what is stored
static bool ced_new;			// not stored yet (New from current settings)
static uint8_t ced_edit = CE_NONE;	// the row whose editor is open
static int32_t ced_val;			// spinner: power level (power_count = the radio's), color code, time-out
static bool ced_typed;
static subtone_t ced_tone;		// spinner: a tone
static bool ced_typed_tone(subtone_t *t);
static char ced_digits[CE_DIGITS + 1];	// typed frequency or ID
static uint8_t ced_len;

static bool tone_same(const subtone_t *a, const subtone_t *b)
{
	return a->type == b->type && (a->type == TONE_NONE || a->value == b->value);
}

// Where each row's field is in channel_t, for the changed mark: offset, size (0 = never changes), and the bit for the
// rows that share alias_tx
#define CE_FIELD(f)	{ offsetof(channel_t, f), sizeof(((channel_t *)0)->f), 0 }
static const struct { uint8_t off, len, bit; } ced_fields[CE_ZONES] = {
	[CE_NAME] = CE_FIELD(name), [CE_MODE] = CE_FIELD(mode), [CE_RX] = CE_FIELD(rx_hz), [CE_TX] = CE_FIELD(tx_hz),
	[CE_POWER] = CE_FIELD(power), [CE_RX_ONLY] = CE_FIELD(rx_only), [CE_BW] = CE_FIELD(bw),
	[CE_RX_TONE] = CE_FIELD(rx_tone), [CE_TX_TONE] = CE_FIELD(tx_tone), [CE_VOX] = CE_FIELD(vox),
	[CE_NAC] = { offsetof(channel_t, nac_rx), 2 * sizeof(uint16_t), 0 },	// nac_rx and nac_tx, side by side
	[CE_TG] = CE_FIELD(talkgroup),
	[CE_CC] = CE_FIELD(color_code), [CE_TS] = CE_FIELD(timeslot), [CE_CONTACT] = CE_FIELD(contact),
	[CE_LIST] = CE_FIELD(listen_list), [CE_ID] = CE_FIELD(dmr_id), [CE_DMO] = CE_FIELD(dmo),
	[CE_BEEP] = CE_FIELD(no_beep), [CE_ALIAS1] = { offsetof(channel_t, alias_tx), 1, CODEPLUG_ALIAS_TS1_TEXT },
	[CE_ALIAS2] = { offsetof(channel_t, alias_tx), 1, CODEPLUG_ALIAS_TS2_TEXT }, [CE_TOT] = CE_FIELD(tot_s),
	[CE_SCAN] = CE_FIELD(scan_skip), [CE_ECO] = CE_FIELD(no_eco),
};

// Whether row r differs from what is stored (the dot), or for CE_COUNT, any row
static bool ced_changed(uint8_t r)
{
	if (ced_new)
		return r == CE_COUNT;
	if (r == CE_COUNT) {
		for (uint8_t i = 0; i < CE_ZONES; i++)
			if (ced_changed(i))
				return true;
		return false;
	}
	if (r >= CE_ZONES)
		return false;
	const uint8_t *a = (const uint8_t *)&ced_ch + ced_fields[r].off, *b = (const uint8_t *)&ced_orig + ced_fields[r].off;
	if (r == CE_NAME)
		return strcmp((const char *)a, (const char *)b) != 0;	// the bytes past the end may differ
	if (r == CE_RX_TONE || r == CE_TX_TONE)
		return !tone_same((const subtone_t *)a, (const subtone_t *)b);
	if (ced_fields[r].bit != 0)
		return ((*a ^ *b) & ced_fields[r].bit) != 0;
	return memcmp(a, b, ced_fields[r].len) != 0;
}

// RX in a receive-only band (the air band, broadcast FM: docs/spec/airband-and-broadcast.md 2): the copy becomes RX
// only and FM (AM there), which is all the radio does there
static void ced_rx_band(channel_t *c)
{
	if (!board_in_band(c->rx_hz, false) || board_in_band(c->rx_hz, true))
		return;
	c->rx_only = true;
	c->tx_hz = c->rx_hz;
	c->mode = RADIO_MODE_FM;
}

// A copy of what the radio uses now (temporary changes included), named after it: "Name 2", or the frequency
static void ced_from_view(void)
{
	const radio_view_t *v = ui_view();
	ced_ch = v->ch;
	if (v->temp & RADIO_TEMP_POWER)
		ced_ch.power = v->power;
	ced_ch.roam = false;
	char name[CODEPLUG_NAME_LEN + 1];
	if (v->vfo) {
		sbuf_t b;
		sb_init(&b, name, sizeof name);
		sb_freq(&b, ced_ch.rx_hz);
	} else {
		zone_copy_name(name, ced_ch.name);	// "Name 2", cut to fit
	}
	strcpy(ced_ch.name, name);
	ced_rx_band(&ced_ch);
}
#endif

static void ced_build(void)
{
	bool dmr = CONFIG_DMR && ced_ch.mode == RADIO_MODE_DMR;
	bool dv = codeplug_mode_digital_voice(ced_ch.mode);	// P25: NAC, talkgroup and unit ID instead
	bool edit = CONFIG_CHANNEL_EDIT;
	bool fresh = false, changed = false;
#if CONFIG_CHANNEL_EDIT
	fresh = ced_new;
	changed = ced_changed(CE_COUNT);
#endif
	ced_n = 0;
	for (uint8_t r = 0; r < CE_COUNT; r++) {
		bool fm_only = r == CE_BW || r == CE_RX_TONE || r == CE_TX_TONE || r == CE_VOX;
		bool dmr_only = r >= CE_CC && r <= CE_ALIAS2;
		if (ced_ch.mode == RADIO_MODE_M17 && (r == CE_TG || r == CE_ID)) continue;
		bool dv_only = r == CE_NAC || r == CE_TG;
		if ((fm_only && (dmr || dv)) || (dmr_only && !dmr && !(dv && r == CE_ID)) || (dv_only && !dv) ||
		    (r == CE_MODE && !(CONFIG_DMR || CONFIG_M17)))
			continue;
		if ((r == CE_ZONES || r == CE_GO || r == CE_SHARE || r == CE_SAVE_NEW || r == CE_DELETE) && fresh)
			continue;
		if (r == CE_SHARE && !CONFIG_ZONE_SHARE)
			continue;
		if ((r == CE_SAVE && !(changed || fresh)) || ((r == CE_SAVE || r == CE_SAVE_NEW || r == CE_DELETE) && !edit))
			continue;
		ced_rows[ced_n++] = r;
	}
}

static void chinfo_enter(void)
{
	ced_num = ui_frame()->arg;
#if CONFIG_CHANNEL_EDIT
	ced_edit = CE_NONE;
	ced_new = ced_from_current;
	ced_from_current = false;
#if CONFIG_ZONE_EDIT
	// Copy here: the channel in use with its temporary changes, as New from current; another one as stored
	uint16_t src = ced_copy_of;
	const radio_view_t *v = ui_view();
	ced_into = src != 0;
	ced_copy_of = 0;
	if (ced_into && (v->vfo || v->channel_number != src)) {
		ced_new = true;
		if (!codeplug_channel(src, &ced_ch))
			ced_num = 0;
		ced_ch.roam = false;
		char name[CODEPLUG_NAME_LEN + 1];
		zone_copy_name(name, ced_ch.name);
		strcpy(ced_ch.name, name);
		ced_orig = ced_ch;
		return;
	}
	ced_new = ced_new || ced_into;
#endif
	if (ced_new) {
		ced_from_view();
		ced_orig = ced_ch;
		return;
	}
#endif
	if (!codeplug_channel(ced_num, &ced_ch))
		ced_num = 0;
#if CONFIG_CHANNEL_EDIT
	ced_orig = ced_ch;
#endif
}

static uint16_t chinfo_count(void)
{
	if (ced_num == 0)
		return 0;
	ced_build();
	return ced_n;
}

static const char *chinfo_title(void)
{
	return ced_ch.name[0] != 0 ? ced_ch.name : TR("New Channel");
}

static zone_t chz_z;	// a zone read to find a channel in it (the Zones row, the channel editor's Zones)

// The zones holding the channel, as much as fits: "Fav, Local, Ga" / "Fav +2" / "3 zones" (text alternates)
static void sb_chinfo_zones(sbuf_t *b, uint16_t num)
{
	char all[24], first[CODEPLUG_NAME_LEN + 1] = "";
	sbuf_t ab;
	sb_init(&ab, all, sizeof all);
	uint16_t found = 0;
	for (uint16_t zi = 0; zi < codeplug_zone_count(); zi++) {
		if (!codeplug_zone(zi, &chz_z))
			continue;
		for (uint16_t pos = 0; pos < chz_z.count; pos++) {
			if (chz_z.channels[pos] != num)
				continue;
			if (found++)
				sb_str(&ab, ", ");
			else
				str_copy(first, sizeof first, chz_z.name);
			sb_str(&ab, chz_z.name);
			break;
		}
	}
	if (found == 0) {
		sb_str(b, TR("None"));
		return;
	}
	sb_str(b, TEXT_ALT_OPEN);
	sb_str(b, all);
	if (found > 1) {
		sb_str(b, TEXT_ALT_NEXT);
		sb_str(b, first);
		sb_str(b, " +");
		sb_u32(b, found - 1u, 1);
	}
	sb_str(b, TEXT_ALT_NEXT);
	sb_u32(b, found, 1);
	sb_str(b, found == 1 ? TR(" zone") : TR(" zones"));
	sb_str(b, TEXT_ALT_CLOSE);
}

static void sb_onoff(sbuf_t *b, bool on)
{
	sb_str(b, on ? TR("On") : TR("Off"));
}

static void sb_ced_power(sbuf_t *b, int32_t level)
{
	sb_str(b, level < 0 || level >= board.power_count ? TR("Radio's") : ui_power_name((uint8_t)level));
}

static void sb_ced_tot(sbuf_t *b, int32_t s)
{
	if (s == 0) {
		sb_str(b, TR("Radio's"));
		return;
	}
	sb_i32(b, s);
	sb_str(b, " s");
}

// The open spinner's value; the stored one in a build without editing (never reached there: nothing is edited)
static int32_t ced_val_or(int32_t stored)
{
#if CONFIG_CHANNEL_EDIT
	(void)stored;
	return ced_val;
#else
	return stored;
#endif
}

static void chinfo_item(uint16_t i, ui_item_t *it)
{
	static const char *const labels[CE_COUNT] = { TRN("Name"), TRN("Number"), TRN("Mode"), "RX", "TX", TRN("Power"),
		TRN("RX only"), TRN("Bandwidth"), TRN("RX CTCSS/DCS"), TRN("TX CTCSS/DCS"), "VOX", "NAC", TRN("Talkgroup"),
		TRN("Color code"),
		TRN("Timeslot"), TRN("TX contact"), TRN("RX group list"), TRN("Own ID"), TRN("Direct mode"),
		TRN("Talk permit beeps"), TRN("Talker alias TS1"), TRN("Talker alias TS2"), TRN("TX time-out"), TRN("Scan"),
		TRN("Battery saver"), TRN("Zones"), TRN("Tune to this ch"), TRN(GLYPH_SHARE " Share channel to..."),
		TRN("Save"), TRN("Save as new channel"), TRN("Delete channel") };
	const channel_t *ch = &ced_ch;
	if (i >= ced_n)
		return;
	uint8_t r = ced_rows[i];
	bool editing = false;
#if CONFIG_CHANNEL_EDIT
	editing = ced_edit == r;
	if (editing)
		it->flags |= UI_ITEM_EDIT;
	if (ced_changed(r))
		it->flags |= UI_ITEM_TEMP;
#endif
	str_copy(it->label, sizeof it->label, r==CE_NAC&&ch->mode==RADIO_MODE_M17?"CAN":tr(labels[r]));
	sbuf_t b;
	sb_init(&b, it->value, sizeof it->value);
#if CONFIG_CHANNEL_EDIT
	if (editing && (r == CE_RX || r == CE_TX || r == CE_ID || r == CE_TG || r == CE_NAC)) {
		// What has been typed so far: "446.07_", "3101_"
		for (uint8_t k = 0; k < ced_len; k++) {
			if (r != CE_ID && r != CE_TG && r != CE_NAC && k == UI_ENTRY_MHZ)
				sb_chr(&b, '.');
			sb_chr(&b, ced_digits[k]);
		}
		sb_chr(&b, '_');
		return;
	}
#endif
	switch (r) {
	case CE_NAME:
		sb_str(&b, ch->name);
		break;
	case CE_NUMBER:
		sb_u32(&b, ced_num, 1);
		break;
	case CE_MODE:
		sb_str(&b, ch->mode == RADIO_MODE_M17 ? "M17" :
			ch->mode == RADIO_MODE_FM ? "FM" : TR("Unsupported"));
		break;
	case CE_NAC: {
		if (ch->mode == RADIO_MODE_M17) {
			// The TX RAN, typed 0-63, and RX: the same one or every RAN (RX RAN 0; OK with nothing typed switches
			// them, as a P25 channel's RX NAC)
			sb_str(&b, "CAN ");
			sb_u32(&b, ch->nac_tx, 1);
			if (ch->nac_rx == 0xFFFF)
				sb_str(&b, TR(", RX any"));
			else if (ch->nac_rx != ch->nac_tx)
				sb_str(&b, TR(", RX other"));
			break;
		}
		// The TX NAC in hex, typed (ced_digit), and RX: the same one or every NAC (OK with nothing typed switches them,
		// or F7E / F7F typed)
		static const char hex[] = "0123456789ABCDEF";
		for (int s = 8; s >= 0; s -= 4)
			sb_chr(&b, hex[(ch->nac_tx >> s) & 0xFu]);
		if (ch->nac_rx == CODEPLUG_NAC_ANY)
			sb_str(&b, TR(", RX any"));
		else if (ch->nac_rx != ch->nac_tx)
			sb_str(&b, TR(", RX other"));
		break;
	}
	case CE_TG:
#if CONFIG_CHANNEL_EDIT
		if (editing && ced_len > 0)
			sb_str(&b, ced_digits);
		else
#endif
			sb_u32(&b, ch->talkgroup, 1);
		break;
	case CE_RX:
		sb_freq(&b, ch->rx_hz);
		break;
	case CE_TX:
		sb_freq(&b, ch->tx_hz);
		if (ch->tx_hz != ch->rx_hz) {
			// The repeater offset: "+5 MHz", "-600 kHz"
			int32_t off_khz = (int32_t)(ch->tx_hz / 1000u) - (int32_t)(ch->rx_hz / 1000u);
			bool mhz = off_khz % 1000 == 0;
			sb_str(&b, off_khz > 0 ? " +" : " ");
			sb_i32(&b, mhz ? off_khz / 1000 : off_khz);
			sb_str(&b, mhz ? " MHz" : " kHz");
		}
		break;
	case CE_POWER:
#if CONFIG_CHANNEL_EDIT
		if (editing) {
			sb_ced_power(&b, ced_val);
			break;
		}
#endif
		sb_ced_power(&b, ch->power == CODEPLUG_POWER_DEFAULT ? -1 : ch->power);
		break;
	case CE_RX_ONLY:
		sb_str(&b, ch->rx_only ? TR("Yes") : TR("No"));
		break;
	case CE_BW:
		sb_str(&b, ch->bw == RADIO_BW_25K ? "25 kHz" : "12.5 kHz");
		break;
	case CE_RX_TONE:
	case CE_TX_TONE: {
		const subtone_t *t = r == CE_RX_TONE ? &ch->rx_tone : &ch->tx_tone;
#if CONFIG_CHANNEL_EDIT
		if (editing) {
			static subtone_t typed;	// static: the UI task's stack has 8 bytes to spare
			t = &ced_tone;
			if (ced_typed_tone(&typed)) {
				t = &typed;
				if (typed.type == TONE_DCS) {	// the digits as typed: "D02"
					sb_chr(&b, 'D');
					sb_str(&b, ced_digits + 1);
					break;
				}
			}
		}
#endif
		sb_tone(&b, t);
		break;
	}
	case CE_VOX:
		sb_onoff(&b, ch->vox);
		break;
	case CE_CC:
		sb_u32(&b, editing ? (uint32_t)ced_val_or(ch->color_code) : ch->color_code, 1);
		break;
	case CE_TS:
		sb_u32(&b, ch->timeslot, 1);
		break;
	case CE_ID:
		if (ch->dmr_id != 0)
			sb_u32(&b, ch->dmr_id, 1);
		else
			sb_str(&b, TR("Radio's"));
		break;
	case CE_DMO:
		sb_onoff(&b, ch->dmo);
		break;
	case CE_BEEP:
		sb_onoff(&b, !ch->no_beep);
		break;
	case CE_ALIAS1:
		sb_onoff(&b, ch->alias_tx & CODEPLUG_ALIAS_TS1_TEXT);
		break;
	case CE_ALIAS2:
		sb_onoff(&b, ch->alias_tx & CODEPLUG_ALIAS_TS2_TEXT);
		break;
	case CE_TOT:
		sb_ced_tot(&b, editing ? ced_val_or(ch->tot_s) : ch->tot_s);
		break;
	case CE_SCAN:
		sb_str(&b, ch->scan_skip ? TR("Skipped") : TR("Scanned"));
		break;
	case CE_ECO:
		sb_onoff(&b, !ch->no_eco);
		break;
	case CE_ZONES:
		sb_chinfo_zones(&b, ced_num);
		break;
	default:
		break;
	}
}

#if CONFIG_CHANNEL_EDIT
static void ui_text_open(char *target, uint8_t max);
static bool discard_contact;	// Unsaved Changes asks about the contact editor's new contact (else a channel)

// The first problem that keeps the copy from being saved, as a toast; NULL when it can be saved
static const char *ced_problem(void)
{
	const channel_t *c = &ced_ch;
	if (c->name[0] == 0)
		return TR("Give it a name first");
	if (!board_in_band(c->rx_hz, false))
		return TR("RX is outside the radio's bands");
	if (!c->rx_only && !board_in_band(c->tx_hz, true))
		return TR("TX can't transmit there: set RX only");
	if (CONFIG_DMR && c->mode == RADIO_MODE_DMR && !board_in_band(c->rx_hz, true))
		return TR("Receive only band: set FM");
	if (CONFIG_DMR && c->mode == RADIO_MODE_DMR && c->contact == 0)
		return TR("Pick a TX contact first");
	if (c->mode != RADIO_MODE_M17 && codeplug_mode_digital_voice(c->mode) && c->talkgroup == 0)
		return TR("Set a talkgroup first");
	return NULL;
}

// Write the copy: as a channel, or for Copy here as a channel put in its zone in the same job (ZONE_OP_ADD_NEW)
static bool ced_write(uint16_t num, bool as_new)
{
#if CONFIG_ZONE_EDIT
	if (ced_into && ced_new && !as_new) {
		ze_op_new(ZONE_OP_ADD_NEW, ced_into_pos)->channel = num;
		ze_op.ch = ced_ch;
		if (!ui_svc_zone_edit(&ze_op))
			return false;
		ced_into = false;
		zone_focus_num = num;	// the zone's list shows it when the editor closes
		return true;
	}
#endif
	(void)as_new;
	return ui_svc_write_channel(num, &ced_ch);
}

static void ced_save(bool as_new)
{
	static char msg[32];
	const char *why = ced_problem();
	if (why != NULL) {
		ui_toast(why);
		return;
	}
	if (!as_new && !ced_new && !ced_changed(CE_COUNT)) {
		ui_toast(TR("No changes to save"));
		return;
	}
	uint16_t num = as_new ? codeplug_free_channel() : ced_num;
	if (num == 0) {
		ui_toast(TR("No free channel"));
		return;
	}
	if (!ced_write(num, as_new)) {
		ui_toast(TR("Couldn't save now: try again"));
		return;
	}
	ced_num = num;
	ui_frame()->arg = num;
	ced_orig = ced_ch;
	sbuf_t b;
	sb_init(&b, msg, sizeof msg);
	sb_str(&b, as_new || ced_new ? TR("Saved as channel ") : TR("Saved channel "));
	sb_u32(&b, num, 1);
	ced_new = false;
	ui_toast(msg);
}

// The tone typed so far. CTCSS in Hz with the last digit the tenths ("885" 88.5); under 60 Hz it reads as whole Hz
// ("67" 67.0, "100" 100.0). A leading 0 types a DCS code ("023" D023N). False: nothing typed
static bool ced_typed_tone(subtone_t *t)
{
	if (ced_len == 0)
		return false;
	uint16_t v = 0;
	if (ced_digits[0] == '0') {
		for (uint8_t i = 1; i < ced_len; i++)
			v = (uint16_t)(v * 8u + (uint16_t)(ced_digits[i] - '0'));
		*t = (subtone_t){ TONE_DCS, v };
	} else {
		for (uint8_t i = 0; i < ced_len; i++)
			v = (uint16_t)(v * 10u + (uint16_t)(ced_digits[i] - '0'));
		*t = (subtone_t){ TONE_CTCSS, v < 600u ? (uint16_t)(v * 10u) : v };
	}
	return true;
}

static void ced_spin(int dir)
{
	switch (ced_edit) {
	case CE_POWER: {
		int32_t n = board.power_count + 1;	// the levels, then the radio's
		ced_val = (ced_val + dir + n) % n;
		break;
	}
	case CE_CC:
		ced_val = (ced_val + dir + 16) % 16;
		break;
	case CE_TOT:
		ced_val += dir * CE_TOT_STEP;
		ced_val = ced_val < 0 ? 0 : ced_val > CE_TOT_MAX ? CE_TOT_MAX : ced_val;
		break;
	case CE_RX_TONE:
	case CE_TX_TONE:
		if (ced_len > 0)
			(void)ced_typed_tone(&ced_tone);	// steps on from what was typed
		ced_len = 0;
		ced_tone = tones_step(&ced_tone, dir);
		break;
	case CE_NAC: {
		// A P25 NAC's last typed digit steps through 0-F, as the keys have no A-F (a step with none typed types a 0)
		if (ced_ch.mode == RADIO_MODE_M17)
			break;
		if (ced_len == 0) {
			ced_val = 0;
			ced_len = 1;
		}
		int32_t n = ((ced_val & 0xF) + dir + 16) % 16;
		ced_val = (ced_val & ~0xF) | n;
		ced_digits[ced_len - 1] = "0123456789ABCDEF"[n];
		ced_digits[ced_len] = 0;
		break;
	}
	default:
		break;
	}
	ced_typed = false;
}

// A digit into the open editor: frequency and ID digits are appended (refused when no band or ID can start so),
// numbers typed as in Settings (starting over when too big). An ID's value builds up in ced_val as it is typed
static void ced_digit(char d)
{
	int32_t v = ced_val * 10 + (d - '0');
	switch (ced_edit) {
	case CE_RX:
	case CE_TX:
		ui_freq_entry_digit(ced_digits, &ced_len, d);
		break;
	case CE_ID:
		if (ced_len >= CE_DIGITS)
			break;
		if (v > CE_ID_MAX) {
			ui_toast(TR("IDs go up to 16776415"));
			break;
		}
		ced_val = v;
		ced_digits[ced_len++] = d;
		ced_digits[ced_len] = 0;
		break;
	case CE_TG:
		if (v > 65535) {
			ui_toast(TR("Talkgroups go up to 65535"));
			break;
		}
		ced_val = v;
		ced_digits[ced_len++] = d;
		ced_digits[ced_len] = 0;
		break;
	case CE_NAC:		// a P25 channel's NAC: 3 hex digits. An NXDN channel's RAN
		if (ced_ch.mode != RADIO_MODE_M17) {
			if (ced_len >= 3)
				break;
			v = ced_val * 16 + (d - '0');
		} else if (v > 15) {
			ui_toast(TR("CAN goes 0 to 15"));
			break;
		}
		ced_val = v;
		ced_digits[ced_len++] = d;
		ced_digits[ced_len] = 0;
		break;
	case CE_RX_TONE:
	case CE_TX_TONE:
		if (ced_len >= 4)
			break;		// 4 digits: "1318", "0754"
		if (ced_len > 0 && ced_digits[0] == '0' && d > '7') {
			ui_toast(TR("DCS codes use digits 0-7"));
			break;
		}
		ced_digits[ced_len++] = d;
		ced_digits[ced_len] = 0;
		break;
	case CE_CC:
	case CE_TOT: {
		int32_t hi = ced_edit == CE_CC ? 15 : CE_TOT_MAX;
		v = ced_typed ? v : d - '0';
		ced_val = v > hi ? d - '0' : v;
		ced_typed = true;
		break;
	}
	default:
		break;
	}
}

// OK in the open editor: the value goes into the copy (or a toast says why not, and the editor stays open)
static void ced_commit(void)
{
	channel_t *c = &ced_ch;
	switch (ced_edit) {
	case CE_RX:
	case CE_TX: {
		if (ced_len == 0)
			break;		// nothing typed: unchanged
		uint32_t hz = ui_freq_entry_hz(ced_digits, ced_len);
		if (hz == 0)
			return;
		if (ced_edit == CE_RX) {
			// TX moves with RX, keeping a repeater's offset within one transmit band; a move to another band (or a
			// channel with no TX frequency) makes TX RX
			uint32_t tx = c->tx_hz - c->rx_hz + hz;
			c->tx_hz = board_offset_carries(c->rx_hz, hz, tx) ? tx : hz;
			c->rx_hz = hz;
			ced_rx_band(c);
		} else {
			c->tx_hz = hz;
		}
		break;
	}
	case CE_ID:
		if (ced_len > 0)
			c->dmr_id = (uint32_t)ced_val;	// 0 = the radio's
		break;
	case CE_TG:
		if (ced_len > 0 && ced_val >= 1)
			c->talkgroup = (uint16_t)ced_val;
		break;
	case CE_NAC:
		if (c->mode == RADIO_MODE_M17) {
			// An NXDN channel's RAN. RX RAN 0 hears every RAN: OK with nothing typed switches RX between that and the
			// TX RAN, and a typed TX RAN leaves RX hearing every RAN when it did
			if (ced_len == 0)
				c->nac_rx = c->nac_rx == 0xFFFF ? c->nac_tx : 0xFFFF;
			else {
				if (c->nac_rx != 0xFFFF)
					c->nac_rx = (uint16_t)ced_val;
				c->nac_tx = (uint16_t)ced_val;
			}
		} else if (ced_len == 0) {
			c->nac_rx = c->nac_rx == CODEPLUG_NAC_ANY ? c->nac_tx : CODEPLUG_NAC_ANY;	// RX: every NAC, or the TX one
		} else if (ced_val == (int32_t)CODEPLUG_NAC_ANY || ced_val == (int32_t)CODEPLUG_NAC_ANY + 1) {
			c->nac_rx = CODEPLUG_NAC_ANY;	// F7E (and F7F, the repeater's "any"): RX hears every NAC, TX is kept
		} else if (ced_val > (int32_t)CODEPLUG_NAC_ANY) {
			ui_toast(TR("NAC goes 000 to F7D"));
			return;
		} else {
			if (c->nac_rx != CODEPLUG_NAC_ANY)
				c->nac_rx = (uint16_t)ced_val;	// RX follows TX, unless it hears every NAC
			c->nac_tx = (uint16_t)ced_val;
		}
		break;
	case CE_POWER:
		c->power = ced_val >= board.power_count ? CODEPLUG_POWER_DEFAULT : (uint8_t)ced_val;
		break;
	case CE_CC:
		c->color_code = (uint8_t)ced_val;
		break;
	case CE_TOT:
		if (ced_val % CE_TOT_STEP != 0) {
			ui_toast(TR("Not changed: 0 to 495, steps of 15"));
			break;
		}
		c->tot_s = (uint16_t)ced_val;
		break;
	case CE_RX_TONE:
	case CE_TX_TONE:
		if (ced_typed_tone(&ced_tone)) {
			if (ced_tone.type == TONE_DCS ? !tones_standard(&ced_tone) : !tones_valid(&ced_tone)) {
				ui_toast(ced_tone.type == TONE_DCS ? TR("Not a DCS code") : TR("CTCSS goes 60.0 to 260.0"));
				return;
			}
			ced_len = 0;
		}
		*(ced_edit == CE_RX_TONE ? &c->rx_tone : &c->tx_tone) = ced_tone;
		break;
	default:
		break;
	}
	ced_edit = CE_NONE;
}

static bool chinfo_key(const input_event_t *ev)
{
	bool press = ev->gesture == GESTURE_PRESS || ev->gesture == GESTURE_LONG;
	if (ced_edit == CE_NONE) {
		if (ev->action == ACT_BACK && ev->gesture == GESTURE_PRESS && ced_num != 0 &&
				(ced_new || ced_changed(CE_COUNT))) {
			discard_contact = false;
			ui_push(SCR_DISCARD, 0);
			return true;
		}
		return false;
	}
	int dir = ev->action == ACT_NAV_UP ? 1 : ev->action == ACT_NAV_DOWN ? -1 :
		ev->action == ACT_ENC_STEP ? (ev->value > 0 ? 1 : -1) : 0;
	if (dir != 0 && ev->gesture != GESTURE_UP) {
		if (ced_edit != CE_RX && ced_edit != CE_TX && ced_edit != CE_ID && ced_edit != CE_TG)
			ced_spin(dir);	// typed fields ignore the knob (a P25 NAC: it steps the last digit), so the focus can't move
		ui_dirty();
		return true;
	}
	if (ev->action >= ACT_DIGIT_0 && ev->action <= ACT_DIGIT_9 && press) {
		ced_digit((char)('0' + ev->action - ACT_DIGIT_0));
		ui_dirty();
		return true;
	}
	if (ev->action == ACT_BACK && ev->gesture == GESTURE_PRESS) {
		if ((ced_edit == CE_RX || ced_edit == CE_TX || ced_edit == CE_ID || ced_edit == CE_TG || ced_edit == CE_NAC ||
		     ced_edit == CE_RX_TONE || ced_edit == CE_TX_TONE) && ced_len > 0) {
			ced_digits[--ced_len] = 0;	// takes the last digit back
			ced_val /= ced_edit == CE_NAC && ced_ch.mode != RADIO_MODE_M17 ? 16 : 10;
		} else {
			ced_edit = CE_NONE;		// leaves the editor, the row unchanged
		}
		ui_dirty();
		return true;
	}
	if (ev->action == ACT_SELECT && press) {
		ced_commit();
		ui_dirty();
		return true;
	}
	return false;
}
#endif

static void chinfo_select(uint16_t i, bool hold)
{
#if CONFIG_CHANNEL_EDIT
	if (i >= ced_n)
		return;
	if (hold) {
		ced_save(false);	// long OK saves, as it makes a quick setting permanent
		return;
	}
	channel_t *c = &ced_ch;
	uint8_t r = ced_rows[i];
	switch (r) {
	case CE_NAME: ui_text_open(c->name, CODEPLUG_NAME_LEN); break;
	case CE_MODE:
		c->mode = c->mode == RADIO_MODE_FM && CONFIG_M17 ? RADIO_MODE_M17 : RADIO_MODE_FM;
		if (c->mode == RADIO_MODE_M17) {
			if (c->nac_tx > 15)
				c->nac_tx = 0;
			if (c->nac_rx == CODEPLUG_NAC_ANY)
				c->nac_rx = 0xFFFF;
			else if (c->nac_rx > 15)
				c->nac_rx = c->nac_tx;
			c->bw = RADIO_BW_12K5;
		}
		ced_build();
		break;
	case CE_NAC:		// typed: a P25 NAC in hex, an NXDN RAN (OK alone switches RX any)
	case CE_TG:
	case CE_RX:
	case CE_TX:
	case CE_ID:
		ced_edit = r;
		ced_len = 0;
		ced_val = 0;
		ced_digits[0] = 0;
		break;
	case CE_POWER:
		ced_edit = r;
		ced_val = c->power < board.power_count ? c->power : board.power_count;
		break;
	case CE_RX_ONLY: c->rx_only = !c->rx_only; break;
	case CE_BW: c->bw = c->bw == RADIO_BW_25K ? RADIO_BW_12K5 : RADIO_BW_25K; break;
	case CE_RX_TONE:
	case CE_TX_TONE:
		if (!CONFIG_SUBTONE) {
			// The spinner would only offer none, and OK would wipe the codeplug's tone
			ui_toast(TR("Tones: not in this build"));
			break;
		}
		ced_edit = r;
		ced_tone = r == CE_RX_TONE ? c->rx_tone : c->tx_tone;
		ced_len = 0;
		break;
	case CE_VOX: c->vox = !c->vox; break;
	case CE_CC:
	case CE_TOT:
		ced_edit = r;
		ced_val = r == CE_CC ? c->color_code : c->tot_s;
		ced_typed = false;
		break;
	case CE_TS: c->timeslot = c->timeslot == 2 ? 1 : 2; break;
	case CE_DMO: c->dmo = !c->dmo; break;
	case CE_BEEP: c->no_beep = !c->no_beep; break;
	case CE_ALIAS1: c->alias_tx ^= CODEPLUG_ALIAS_TS1_TEXT; break;
	case CE_ALIAS2: c->alias_tx ^= CODEPLUG_ALIAS_TS2_TEXT; break;
	case CE_SCAN: c->scan_skip = !c->scan_skip; break;
	case CE_ECO: c->no_eco = !c->no_eco; break;
	case CE_NUMBER: ui_toast(TR("Save as new channel to use another number")); break;
#if CONFIG_ZONE_EDIT
	case CE_ZONES: ui_push(SCR_CHZONES, ced_num); break;
#else
	case CE_ZONES: ui_toast(TR("Zones: change them with qdmr for now")); break;
#endif
	case CE_GO:
		if (ced_changed(CE_COUNT))
			ui_toast(TR("Save first: hold OK"));
		else
			chlist_tune(ced_num);
		break;
	case CE_SAVE: ced_save(false); break;
	case CE_SAVE_NEW: ced_save(true); break;
	case CE_DELETE: ui_push(SCR_CHDELETE, ced_num); break;
	default: break;
	}
	ui_dirty();
#else
	(void)i;
	(void)hold;
	chlist_tune(ui_frame()->arg);
#endif
}

static const ui_scr_def_t chinfo_screen = {
	.name = "channel", .flags = SF_LIST | SF_FIELD, .empty = TRN("Empty channel"),
#if CONFIG_CHANNEL_EDIT
	.hint = TRN(TEXT_ALT_OPEN "OK change  hold OK save  Back" TEXT_ALT_NEXT "OK change  hold OK save" TEXT_ALT_CLOSE),
	.key = chinfo_key,
#else
	.hint = TRN("OK tunes to it  Back"),
#endif
	.count = chinfo_count, .item = chinfo_item, .select = chinfo_select, .title = chinfo_title,
	.enter = chinfo_enter,
};

#if CONFIG_CHANNEL_EDIT
static uint16_t chdelete_count(void) { return 2; }

static void chdelete_item(uint16_t i, ui_item_t *it)
{
	str_copy(it->label, sizeof it->label, i == 0 ? TR("No, keep it") : TR("Yes, delete"));
	if (i == 1)
		it->flags |= UI_ITEM_WARN;
}

static void chdelete_select(uint16_t i, bool hold)
{
	(void)hold;
	if (i == 0) {
		ui_pop();
	} else if (ui_svc_delete_channel(ui_frame()->arg)) {
		ui_pop();
		ui_pop();
		ui_toast(TR("Deleting channel"));
	} else {
		ui_toast(TR("Couldn't delete the channel"));
	}
}

static const char *chdelete_body(void)
{
	return TR("Delete this channel and remove it from every zone?");
}

static const ui_scr_def_t chdelete_screen = {
	.name = "channel-delete", .flags = SF_LIST, .prefix = TRN("Delete channel"),
	.hint = TRN("OK choose  Back cancel"),
	.count = chdelete_count, .item = chdelete_item, .select = chdelete_select, .body = chdelete_body,
};

// ======================= Leaving the channel editor with changes (defaults to keep editing; P7) ===============

static uint16_t discard_count(void) { return 2; }

static void discard_item(uint16_t i, ui_item_t *it)
{
	str_copy(it->label, sizeof it->label, i == 0 ? TR("No, keep editing") : TR("Yes, discard"));
	if (i == 1)
		it->flags |= UI_ITEM_WARN;
}

static void discard_select(uint16_t i, bool hold)
{
	(void)hold;
	ui_pop();		// this question
	if (i == 1)
		ui_pop();	// and the channel
}

static const char *discard_body(void)
{
	return discard_contact ? TR("Discard this new contact?") : TR("Discard the changes to this channel?");
}

static const ui_scr_def_t discard_screen = {
	.name = "discard", .flags = SF_LIST, .prefix = TRN("Unsaved Changes"),
	.hint = TRN(TEXT_ALT_OPEN "OK choose  Back keep editing" TEXT_ALT_NEXT "OK choose  Back" TEXT_ALT_CLOSE),
	.count = discard_count, .item = discard_item, .select = discard_select, .body = discard_body,
};

#endif // CONFIG_CHANNEL_EDIT
#endif // CONFIG_CHANNEL_LIST

#if CONFIG_CHANNEL_EDIT
// ======================= Text entry: keypad letters (ui.md 2.1) =======================
// Phone keypad: in ABC / abc, 2-9 cycle their letters, the digit, then the letters in the other case (ABC: A B C 2 a b
// c); 1 cycles punctuation and 0 a space, 0 and a few signs. The same key within 1 s takes the next one, another key
// or the pause moves on. In 123 a digit types itself, as a held digit does
// in any mode. # cycles ABC -> 123 -> abc. BACK deletes the last character and leaves once the field is empty, without
// a change (the phone convention; the one exception to P4, principles 4); * or Left delete too. OK keeps. Without a
// keypad Up / Down step the last character (down past the space deletes it), OK adds one and long OK keeps

#define TEXT_MULTI_MS	1000u

static char *text_target;
static void (*text_done)(void);		// called once the text is kept (NULL: none)
static uint8_t text_max;
static uint8_t text_len;
static int8_t text_key = -1;		// the key that typed the last character, -1 = none to cycle
static uint8_t text_idx;
static uint32_t text_ms;
enum { TEXT_UPPER, TEXT_DIGITS, TEXT_LOWER, TEXT_MODES };	// the order # cycles through
static uint8_t text_mode;

#if CONFIG_CHANNEL_EDIT
static void ui_text_open(char *target, uint8_t max)
{
	text_target = target;
	text_done = NULL;
	text_max = max < CODEPLUG_NAME_LEN ? max : CODEPLUG_NAME_LEN;
	ui_push(SCR_TEXT, 0);
}
#endif

static void text_enter(void)
{
	if (text_target != NULL)
		strncpy(text_buf, text_target, text_max);
	else
		text_buf[0] = 0;
	text_buf[text_max] = 0;
	text_len = (uint8_t)strlen(text_buf);
	text_key = -1;
	text_mode = TEXT_UPPER;
}

#if CONFIG_CHANNEL_EDIT
static uint16_t text_count(void) { return 1; }

static void text_item(uint16_t i, ui_item_t *it)
{
	(void)i;
	sbuf_t b;
	sb_init(&b, it->label, sizeof it->label);
	sb_str(&b, text_buf);
	sb_chr(&b, '_');	// the cursor; the mode is in the title
}
#endif


static void text_changed(void)
{
	ui_dirty();
}

static char text_case(char c)
{
	return text_mode == TEXT_LOWER && c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
}

// The keys with letters cycle letters, digit, then the letters again in the other case; the others their set as is
static uint8_t text_cycle_len(const char *set)
{
	uint8_t n = (uint8_t)strlen(set);
	return set[0] >= 'A' && set[0] <= 'Z' ? (uint8_t)(2u * n - 1u) : n;
}

static char text_char(const char *set, uint8_t idx)
{
	uint8_t n = (uint8_t)strlen(set);
	if (!(set[0] >= 'A' && set[0] <= 'Z'))
		return set[idx];
	if (idx < n)
		return idx == n - 1u ? set[idx] : text_case(set[idx]);	// the letters in the mode's case, then the digit
	char c = set[idx - n];
	return text_mode == TEXT_LOWER ? c : (char)(c - 'A' + 'a');	// then the other case
}

static bool text_append(char c)
{
	if (text_len >= text_max) {
		ui_toast(TR("That's the most it can hold"));
		return false;
	}
	text_buf[text_len++] = c;
	text_buf[text_len] = 0;
	return true;
}

static bool text_key_fn(const input_event_t *ev)
{
	// A key's characters within the timing window: its letters, its digit, its letters in the other case (text_char)
	static const char *const keys[10] = { " 0+#*", ".-/,'&()@_!?1", "ABC2", "DEF3", "GHI4", "JKL5", "MNO6", "PQRS7",
		"TUV8", "WXYZ9" };
	bool press = ev->gesture == GESTURE_PRESS || ev->gesture == GESTURE_LONG;
	uint32_t now = hal_millis();
	if (ev->action >= ACT_DIGIT_0 && ev->action <= ACT_DIGIT_9 && press) {
		int8_t k = (int8_t)(ev->action - ACT_DIGIT_0);
		const char *set = keys[k];
		if (text_mode == TEXT_DIGITS || ev->gesture == GESTURE_LONG) {
			text_append((char)('0' + k));	// 123, or a held digit: the digit itself
			text_key = -1;
		} else if (k == text_key && text_len > 0 && now - text_ms < TEXT_MULTI_MS) {
			text_idx = (uint8_t)((text_idx + 1u) % text_cycle_len(set));
			text_buf[text_len - 1] = text_char(set, text_idx);
			text_key = k;
		} else {
			text_idx = 0;
			text_key = text_append(text_char(set, 0)) ? k : -1;
		}
		text_ms = now;
		text_changed();
		return true;
	}
	int dir = ev->action == ACT_NAV_UP ? 1 : ev->action == ACT_NAV_DOWN ? -1 :
		ev->action == ACT_ENC_STEP ? (ev->value > 0 ? 1 : -1) : 0;
	if (dir != 0 && ev->gesture != GESTURE_UP) {
		// Step the last character through the printable ones (the way in without a keypad); down past the space
		// deletes it
		if (text_len == 0)
			text_append('A');
		else if (dir < 0 && text_buf[text_len - 1] == ' ')
			text_buf[--text_len] = 0;
		else
			text_buf[text_len - 1] = (char)(' ' + (text_buf[text_len - 1] - ' ' + dir + 95) % 95);
		text_key = -1;
		text_changed();
		return true;
	}
	if (ev->action == ACT_CONTEXT && ev->gesture == GESTURE_PRESS) {
		text_mode = (uint8_t)((text_mode + 1u) % TEXT_MODES);
		text_key = -1;	// the next key starts a new character
		text_changed();
		return true;
	}
	if (ev->action == ACT_BACK && ev->gesture == GESTURE_PRESS && text_len == 0) {
		ui_pop();	// an empty field: BACK leaves, the name as it was
		return true;
	}
	if (((ev->action == ACT_BACK || ev->action == ACT_NAV_LEFT) && ev->gesture == GESTURE_PRESS) ||
			(ev->action == ACT_QUICK && press)) {	// a slow * deletes too (it doesn't lock here)
		if (text_len > 0)
			text_buf[--text_len] = 0;
		text_key = -1;
		text_changed();
		return true;
	}
	if (ev->action == ACT_SELECT && press) {
		if (!(board.caps & CAP_KEYPAD) && ev->gesture == GESTURE_PRESS) {
			text_append(text_len > 0 ? text_buf[text_len - 1] : 'A');	// the next character, started as this one
		} else {
			strcpy(text_target, text_buf);
			ui_pop();
			if (text_done != NULL)
				text_done();
		}
		text_changed();
		return true;
	}
	return false;
}

// The title carries the mode, so it shows however long the name is: "Name · ABC"
static const char *text_title(void)
{
	static const char *const modes[TEXT_MODES] = { "ABC", "123", "abc" };
	return modes[text_mode];
}

#if CONFIG_CHANNEL_EDIT
static const ui_scr_def_t text_screen = {
	.name = "text", .flags = SF_LIST, .prefix = TRN("Name"), .title = text_title,
	.hint = TRN(TEXT_ALT_OPEN "Back delete  # ABC/123/abc  OK keep" TEXT_ALT_NEXT "Back del  # mode  OK" TEXT_ALT_CLOSE),
	.count = text_count, .item = text_item, .key = text_key_fn, .enter = text_enter,
};
#endif

#endif // CONFIG_CHANNEL_EDIT || CONFIG_DMR_SMS

#if CONFIG_CHANNEL_LIST
#if CONFIG_CHANNEL_EDIT

#if CONFIG_ZONE_EDIT
// ======================= The zone editor (ui.md 2.2) =======================
// Channel actions (arg: the position), Add channel, Zone options and Delete zone work on zone ze_zone. ze_z is the
// zone as these screens show it, read when one opens and changed as the change is posted, so it is right before the
// job has run

static zone_t ze_z;

static void ze_read(void)
{
	if (!codeplug_zone(ze_zone, &ze_z))
		memset(&ze_z, 0, sizeof ze_z);
}

static const char *ze_title(void) { return ze_z.name; }

// --- Channel actions ---
enum { ZC_UP, ZC_DOWN, ZC_COPY, ZC_EDIT,
	ZC_REMOVE, ZC_COUNT };
static char zc_name[CODEPLUG_NAME_LEN + 1];

static void zchan_enter(void)
{
	static channel_t ch;
	ze_read();
	uint16_t pos = ui_frame()->arg;
	zc_name[0] = 0;
	if (pos < ze_z.count && codeplug_channel(ze_z.channels[pos], &ch))
		strcpy(zc_name, ch.name);
}

static uint16_t zchan_count(void) { return ZC_COUNT; }

static void zchan_item(uint16_t i, ui_item_t *it)
{
	static const char *const names[ZC_COUNT] = {
		[ZC_UP] = TRN("Move up"), [ZC_DOWN] = TRN("Move down"), [ZC_COPY] = TRN("Copy here"),
		[ZC_EDIT] = TRN("Edit channel"),
		[ZC_REMOVE] = TRN("Remove from zone") };
	str_copy(it->label, sizeof it->label, tr(names[i]));
	item_num(it, (uint16_t)(i + 1u));
	if (i == ZC_REMOVE)
		it->flags |= UI_ITEM_WARN;
}

static void zchan_select(uint16_t i, bool hold)
{
	(void)hold;
	uint16_t pos = ui_frame()->arg;
	if (pos >= ze_z.count)
		return;
	uint16_t num = ze_z.channels[pos];
	if (i == ZC_UP || i == ZC_DOWN) {
		int dir = i == ZC_UP ? -1 : 1;
		if (zone_list_move(&ze_z, pos, dir) != ZONE_EDIT_OK) {
			ui_toast(dir < 0 ? TR("Already first") : TR("Already last"));
			return;
		}
		ze_op_new(ZONE_OP_MOVE, pos)->dir = (int8_t)dir;
		if (!ze_post()) {
			zone_list_move(&ze_z, (uint16_t)(pos + dir), -dir);
			return;
		}
		pos = (uint16_t)(pos + dir);
		ui_frame()->arg = pos;		// the menu stays on the channel: the digit again moves it on
		zone_focus_num = num;
		sbuf_t b;
		sb_init(&b, ze_msg, sizeof ze_msg);
		sb_str(&b, TR("Position "));
		sb_u32(&b, pos + 1u, 1);
		sb_str(&b, TR(" of "));
		sb_u32(&b, ze_z.count, 1);
		ui_toast(ze_msg);
	} else if (i == ZC_COPY) {
		uint16_t free = codeplug_free_channel();
		if (free == 0) {
			ui_toast(TR("No free channel"));
		} else if (ze_z.count >= codeplug_zone_capacity()) {
			ui_toast(TR("Zone full"));
		} else {
			ced_copy_of = num;
			ced_into_pos = (uint16_t)(pos + 1u);
			ui_pop();	// the editor takes the menu's place: the stack stays within its six frames
			ui_push(SCR_CHANNEL, free);
		}
	} else if (i == ZC_EDIT) {
		ui_pop();
		ui_push(SCR_CHANNEL, num);
	} else if (i == ZC_REMOVE) {
		ze_op_new(ZONE_OP_REMOVE, pos);
		if (ze_post()) {
			ui_pop();
			ze_toast(TR("Removed from "), ze_z.name);
		}
	}
}

static const char *zchan_title(void) { return zc_name; }

static const ui_scr_def_t zchan_screen = {
	.name = "zchannel", .flags = SF_LIST | SF_NUMBERED, .hint = CONFIG_ZONE_SHARE ? TRN("1-6 pick  Back") : TRN("1-5 pick  Back"),
	.count = zchan_count, .item = zchan_item, .select = zchan_select, .title = zchan_title, .enter = zchan_enter,
};

// --- Add channel: the stored channels the zone doesn't hold (raw r: channel r + 1) ---
static uint16_t zpick_raw_count(void)
{
	ze_read();
	return codeplug_channel_count();
}

static channel_t zpick_ch;	// the channel zpick_raw_label read last

static bool zpick_raw_label(uint16_t raw, char *buf, uint8_t len)
{
	uint16_t num = (uint16_t)(raw + 1u);
	if (zone_list_find(&ze_z, num) >= 0 || !codeplug_channel(num, &zpick_ch))
		return false;
	sbuf_t b;
	sb_init(&b, buf, len);
	sb_u32(&b, num, 1);
	sb_chr(&b, ' ');
	sb_str(&b, zpick_ch.name);
	return true;
}

static void zpick_item(uint16_t i, ui_item_t *it)
{
	if (!zpick_raw_label(ui_filter_raw(i), it->label, sizeof it->label))
		return;
	sbuf_t b;
	sb_init(&b, it->value, sizeof it->value);
	sb_channel_row(&b, &zpick_ch);
}

static void zpick_select(uint16_t i, bool hold)
{
	(void)hold;
	uint16_t num = (uint16_t)(ui_filter_raw(i) + 1u);
	const radio_view_t *v = ui_view();
	if (ze_z.count >= codeplug_zone_capacity()) {
		ui_toast(TR("Zone full"));
		return;
	}
	// After the channel in use when this is the zone in use, else at the end
	bool here = !v->vfo && v->zone_index == ze_zone && v->zone_pos < ze_z.count;
	ze_op_new(ZONE_OP_INSERT, here ? (uint16_t)(v->zone_pos + 1u) : ze_z.count)->channel = num;
	if (!ze_post())
		return;
	zone_focus_num = num;
	ui_pop();
	ui_filter_reset();	// the whole list again (the 2 typed to find Add channel), on the channel added
	ui_menus_resume();
	ze_toast(TR("Added to "), ze_z.name);
}

static const ui_scr_def_t zpick_screen = {
	.name = "zpick", .flags = SF_LIST | SF_FILTER, .prefix = TRN("Add Channel"),
	.hint = TRN(TEXT_ALT_OPEN "OK add  0-9 find  Back" TEXT_ALT_NEXT "OK add  Back" TEXT_ALT_CLOSE),
	.empty = TRN("Every channel is in it"),
	.count = ui_filter_count, .item = zpick_item, .select = zpick_select,
	.raw_count = zpick_raw_count, .raw_label = zpick_raw_label,
};

// --- The channel editor's Zones: every zone, and whether the channel is in it; OK puts it in or takes it out ---
static int chz_pos(uint16_t zi)
{
	if (!codeplug_zone(zi, &chz_z))
		return -2;
	return zone_list_find(&chz_z, ui_frame()->arg);
}

static uint16_t chz_count(void) { return codeplug_zone_count(); }

static void chz_item(uint16_t i, ui_item_t *it)
{
	int at = chz_pos(i);
	if (at == -2)
		return;
	strcpy(it->label, chz_z.name);
	str_copy(it->value, sizeof it->value, at >= 0 ? TR("In") : "-");
}

static void chz_select(uint16_t i, bool hold)
{
	(void)hold;
	int at = chz_pos(i);
	if (at == -2)
		return;
	if (at < 0 && chz_z.count >= codeplug_zone_capacity()) {
		ui_toast("Zone full");
		return;
	}
	// The zone screens' op, for this zone (ze_zone stays the open zone's: this editor may sit on its list)
	zone_op_t *op = ze_op_new(at >= 0 ? ZONE_OP_REMOVE : ZONE_OP_INSERT, at >= 0 ? (uint16_t)at : chz_z.count);
	op->zone = i;
	op->channel = ui_frame()->arg;
	if (ze_post())
		ze_toast(at >= 0 ? TR("Removed from ") : TR("Added to "), chz_z.name);
}

static const ui_scr_def_t chz_screen = {
	.name = "chzones", .flags = SF_LIST, .prefix = "In Zones", .hint = "OK in/out  Back", .empty = "No zones",
	.count = chz_count, .item = chz_item, .select = chz_select,
};

// --- Zone options ---
enum { ZO_RENAME, ZO_COPY,
	ZO_DELETE, ZO_COUNT };

static uint16_t zopt_count(void) { return ZO_COUNT; }

static void zopt_item(uint16_t i, ui_item_t *it)
{
	static const char *const names[ZO_COUNT] = {
		[ZO_RENAME] = TRN("Rename"), [ZO_COPY] = TRN("Copy as new zone"),
		[ZO_DELETE] = TRN("Delete zone") };
	str_copy(it->label, sizeof it->label, tr(names[i]));
	item_num(it, (uint16_t)(i + 1u));
	if (i == ZO_DELETE)
		it->flags |= UI_ITEM_WARN;
}

static void ze_renamed(void)
{
	if (ze_name[0] == 0) {
		ui_toast(TR("Give it a name first"));
		return;
	}
	strcpy(ze_op_new(ZONE_OP_RENAME, 0)->name, ze_name);
	if (ze_post()) {
		strcpy(ze_z.name, ze_name);
		ui_toast(TR("Renamed"));
	}
}

// Zones > Create New Zone: named in the text entry, then written empty; Zones focuses it, and its list's first row adds
// channels
static void zones_new_named(void)
{
	if (ze_name[0] == 0) {
		ui_toast("Give it a name first");
		return;
	}
	strcpy(ze_op_new(ZONE_OP_NEW, 0)->name, ze_name);
	if (!ze_post())
		return;
	zones_new_pending = true;
	ze_toast(TR("Added "), ze_name);
}

static void zones_new(void)
{
	if (codeplug_zone_count() >= CODEPLUG_ZONES_MAX && codeplug_zones_stored()) {
		ui_toast(TR("No room for another zone"));
		return;
	}
	ze_name[0] = 0;
	ui_text_open(ze_name, CODEPLUG_NAME_LEN);
	text_done = zones_new_named;
}

static void zopt_select(uint16_t i, bool hold)
{
	(void)hold;
	if (i == ZO_RENAME) {
		strcpy(ze_name, ze_z.name);
		ui_text_open(ze_name, CODEPLUG_NAME_LEN);
		text_done = ze_renamed;
	} else if (i == ZO_COPY) {
		if (codeplug_zone_count() >= CODEPLUG_ZONES_MAX) {
			ui_toast(TR("No room for another zone"));
			return;
		}
		ze_op_new(ZONE_OP_COPY, 0);
		if (!ze_post())
			return;
		// Back to Zones: the copy can take a slot ahead of this zone and move its index
		ui_pop();
		ui_pop();
		char name[CODEPLUG_NAME_LEN + 1];
		zone_copy_name(name, ze_z.name);
		ze_toast(TR("Copied as "), name);
	} else if (!codeplug_zones_stored()) {
		ui_toast(TR("It holds every channel: it stays"));
	} else {
		ui_push(SCR_ZONEDEL, 0);
	}
}

static const ui_scr_def_t zopt_screen = {
	.name = "zoneedit", .flags = SF_LIST | SF_NUMBERED, .prefix = TRN("Zone"),
	.hint = CONFIG_ZONE_SHARE ? TRN("1-4 pick  Back") : TRN("1-3 pick  Back"),
	.count = zopt_count, .item = zopt_item, .select = zopt_select, .title = ze_title, .enter = ze_read,
};

// --- Delete zone (destructive: asks, defaults to No; P7) ---
static uint16_t zdel_count(void) { return 2; }

static void zdel_item(uint16_t i, ui_item_t *it)
{
	str_copy(it->label, sizeof it->label, i == 0 ? TR("No, keep it") : TR("Yes, delete"));
	if (i == 1)
		it->flags |= UI_ITEM_WARN;
}

static void zdel_select(uint16_t i, bool hold)
{
	(void)hold;
	ui_pop();		// this question
	if (i == 0)
		return;
	ze_op_new(ZONE_OP_DELETE, 0);
	if (!ze_post())
		return;
	ui_pop();		// Zone options
	ui_pop();		// and the zone's list: back to Zones
	ze_toast(TR("Deleted "), ze_z.name);
}

// The codeplug changed and nothing of ours is queued (our last change written, or refused, or a CPS write): the
// editor's screens show the zone as stored. Channel actions stay on their channel, found by number; it closes when
// the channel is no longer in the zone
void ui_menus_codeplug(void)
{
	ui_frame_t *f = ui_frame();
	if (f->scr != SCR_ZCHANNEL && f->scr != SCR_ZPICK && f->scr != SCR_ZONEEDIT && f->scr != SCR_ZONEDEL)
		return;
	uint16_t num = f->scr == SCR_ZCHANNEL && f->arg < ze_z.count ? ze_z.channels[f->arg] : 0;
	ze_read();
	if (f->scr == SCR_ZCHANNEL) {
		int at = zone_list_find(&ze_z, num);
		if (at < 0) {
			ui_pop();
			return;
		}
		f->arg = (uint16_t)at;
		zone_focus_num = num;
	}
}

static const char *zdel_body(void)
{
	sbuf_t b;
	sb_init(&b, ze_msg, sizeof ze_msg);
	sb_str(&b, TR("Delete "));
	sb_str(&b, ze_z.name);
	sb_str(&b, TR("? Channels stay."));
	return ze_msg;
}

static const ui_scr_def_t zdel_screen = {
	.name = "zonedel", .flags = SF_LIST, .prefix = TRN("Delete Zone"), .hint = TRN("OK choose  Back cancel"),
	.count = zdel_count, .item = zdel_item, .select = zdel_select, .body = zdel_body,
};
#endif


#endif
#endif

// ======================= Info pages =======================

enum { INFO_POSITION, INFO_HOTSPOT, INFO_BATTERY, INFO_FIRMWARE, INFO_CODEPLUG, INFO_USB_NET,
       INFO_SFR };

static const char *const info_titles[] = { TRN("Position"), TRN("Hotspot"), TRN("Battery"), TRN("About"),
					   TRN("Codeplug"), TRN("USB Network"), TRN("Single-Freq Repeater") };

// The USB personality can be switched here (the host link's network adapter, CONFIG_USB_NETWORK)
#define USB_NET_UI	(CONFIG_USB_NETWORK && CONFIG_HOSTLINK)

static const char *info_title(void)
{
	uint16_t a = ui_frame()->arg;
	return a == INFO_BATTERY && board_volts() == BOARD_VOLTS_SUPPLY ? TR("Supply") : tr(info_titles[a]);
}

// mV as volts with one decimal: 13.8 V
static void sb_volts(sbuf_t *b, uint16_t mv)
{
	uint32_t dv = (mv + 50u) / 100u;
	sb_u32(b, dv / 10u, 1);
	sb_chr(b, '.');
	sb_u32(b, dv % 10u, 1);
	sb_str(b, " V");
}

static const char *info_body(void)
{
	static char t[200];
	sbuf_t b;
	sb_init(&b, t, sizeof t);
	switch (ui_frame()->arg) {
	case INFO_POSITION: {
		ui_position_t p;
		if (!ui_svc_position(&p)) {
			sb_str(&b, TR("No GPS, or it is off (Settings > Location)"));
		} else if (!p.fix) {
			sb_str(&b, TR("No fix yet. Go outside, away from buildings."));	// GGA counts only satellites a fix uses
		} else {
			sb_str(&b, TR("Fix, "));
			sb_u32(&b, p.sats, 1);
			sb_str(&b, TR(" satellites"));
			sb_chr(&b, '\n');
			sb_coord(&b, p.lat_e7, 'N', 'S');
			sb_chr(&b, ' ');
			sb_coord(&b, p.lon_e7, 'E', 'W');
			if (p.alt_ok) {
				sb_chr(&b, '\n');
				sb_str(&b, TR("Altitude "));
				sb_i32(&b, p.alt_m);
				sb_str(&b, " m");
			}
		}
		break;
	}
	case INFO_HOTSPOT:
		sb_str(&b, TR("Hotspot: not in this build"));
		break;
	case INFO_BATTERY:
		if (board_volts() == BOARD_VOLTS_SUPPLY) {	// a reading only: no charge level or warning
			sb_volts(&b, ui_view()->supply_mv);
			break;
		}
		sb_u32(&b, ui_st.battery_pct, 1);
		sb_str(&b, ui_st.battery_pct <= (ui_st.low_batt_pct ? ui_st.low_batt_pct : 15) ? TR("%: charge soon") : "%");
		break;
	case INFO_CODEPLUG: {
		static const char *const src[] = { TRN("None: built-in channels"), TRN("Built-in channels"),
						   TRN("Written by the CPS") };
		codeplug_source_t s = codeplug_source();
		sb_str(&b, (unsigned)s < 3 ? tr(src[s]) : "?");
		sb_chr(&b, '\n');
		if (codeplug_foreign()) {	// another firmware's codeplug, left alone until the CPS writes one
			sb_str(&b, TR("Other firmware's codeplug: ignored"));
			sb_chr(&b, '\n');
		}
		sb_str(&b, TR("Zones: "));
		sb_u32(&b, codeplug_zone_count(), 1);
		sb_chr(&b, '\n');
		sb_str(&b, TR("Channels: up to "));
		sb_u32(&b, codeplug_channel_count(), 1);
		break;
	}
	case INFO_USB_NET:
#if USB_NET_UI
		if (hostlink_network_mode())
			sb_str(&b, TR("USB is a network adapter: IP over DMR. qdmr and the hotspot are off until you switch "
				"back."));
		else
			sb_str(&b, TR("USB is a serial port: qdmr, hotspot. As a network adapter it carries IP over DMR."));
#else
		sb_str(&b, TR("USB network: not in this build"));
#endif
		break;
	case INFO_SFR: {
		sb_str(&b, TR("Single-freq repeater: not in this build"));
		break;
	}
	default:
		break;
	}
	return t;
}

static uint16_t info_count(void)
{
#if USB_NET_UI
	if (ui_frame()->arg == INFO_USB_NET)
		return 1;
#endif
	return 0;
}

static void info_item(uint16_t i, ui_item_t *it)
{
#if USB_NET_UI
	if (ui_frame()->arg == INFO_USB_NET) {
		str_copy(it->label, sizeof it->label, hostlink_network_mode() ? TR("Switch USB to serial") :
			 TR("Switch USB to network"));
		return;
	}
#endif
	str_copy(it->label, sizeof it->label, TR("Stop hotspot"));
}

static void info_select(uint16_t i, bool hold)
{
	(void)hold;
#if USB_NET_UI
	if (ui_frame()->arg == INFO_USB_NET) {
		bool net = !hostlink_network_mode();
		if (!hostlink_request_usb_network(net)) {
			ui_toast(TR("No USB network on this radio"));
		} else {
			ui_go_home();
			ui_toast(net ? TR("USB is a network adapter now") : TR("USB is a serial port again"));
		}
		return;
	}
#endif
}

static const ui_scr_def_t info_screen = {
	.name = "info", .flags = SF_LIST | SF_FIELD, .hint = TRN("Back"), .count = info_count, .item = info_item,
	.select = info_select, .title = info_title, .body = info_body,
};

// ======================= My radio: DMR ID and names (Settings > My radio) =======================
// The radio's own DMR ID (CONFIG_DMR) and the codeplug's name fields: the radio name and the two boot screen lines,
// all saved in the codeplug (a CPS read shows them). On the ID row digits type a new one, OK writes it. Typed as the
// channel editor's Own ID (ui.md 2.1): a digit past CODEPLUG_DMR_ID_MAX is refused, BACK takes the last digit back and
// with none typed leaves the ID unchanged. OK on a name row opens the text entry; keeping it writes the name

enum {
	MY_NAME, MY_BOOT1, MY_BOOT2, MY_COUNT };
static const uint8_t my_names[] = { [MY_NAME] = UI_NAME_RADIO, [MY_BOOT1] = UI_NAME_BOOT1, [MY_BOOT2] = UI_NAME_BOOT2 };
static char my_text[CODEPLUG_NAME_LEN + 1];	// the name being typed, then shown until the codeplug's serial moves on
static uint8_t my_text_row = MY_COUNT;		// the row my_text belongs to (MY_COUNT: none)
static uint32_t my_text_serial;


static uint16_t dmr_id_count(void) { return MY_COUNT; }

void ui_menus_save_failed(void)
{
#if CONFIG_ZONE_EDIT
	zones_new_pending = false;	// nothing to focus
#endif
	my_text_row = MY_COUNT;		// the stored name shows again
}

static void dmr_id_item(uint16_t i, ui_item_t *it)
{
	sbuf_t b;
	sb_init(&b, it->value, sizeof it->value);
	static const char *const labels[] = { [MY_NAME] = TRN("Radio name"), [MY_BOOT1] = TRN("Boot 1"),
		[MY_BOOT2] = TRN("Boot 2") };
	str_copy(it->label, sizeof it->label, tr(labels[i]));
	char name[CODEPLUG_NAME_LEN + 1];
	if (my_text_row == i && codeplug_serial() == my_text_serial)
		strcpy(name, my_text);
	else
		ui_svc_name_get(my_names[i], name);
	sb_str(&b, name[0] != 0 ? name : TR("None"));
}

static const char *dmr_id_body(void)
{
	static char t[48];
	sbuf_t b;
	sb_init(&b, t, sizeof t);
	sb_str(&b, TR("Saved in the codeplug"));
	sb_chr(&b, '\n');
	sb_str(&b, TR("Radio: "));
	sb_str(&b, board.name);		// as the info page it replaces
	return t;
}

#if CONFIG_CHANNEL_EDIT
static void my_text_kept(void)
{
	my_text_serial = codeplug_serial();	// before the job: the storage task may finish it before we look again
	if (!ui_svc_name_set(my_names[my_text_row], my_text)) {
		my_text_row = MY_COUNT;
		ui_toast("Couldn't save: try again");
		return;
	}
	ui_toast(TR("Saved"));
}
#endif


static void dmr_id_select(uint16_t i, bool hold)
{
	(void)hold;
	if (i >= MY_COUNT)
		return;
#if CONFIG_CHANNEL_EDIT
	ui_svc_name_get(my_names[i], my_text);
	my_text_row = (uint8_t)i;
	my_text_serial = codeplug_serial() - 1u;	// shown from the codeplug until it is kept
	ui_text_open(my_text, my_names[i] == UI_NAME_RADIO ? CODEPLUG_RADIO_NAME_LEN : CODEPLUG_NAME_LEN);
	text_done = my_text_kept;
#else
	ui_toast(TR("Typing: not in this build"));
#endif
}

static bool dmr_id_key(const input_event_t *ev)
{
	(void)ev;
	return false;
}

static void dmr_id_enter(void)
{
}

static const ui_scr_def_t dmr_id_screen = {
	.name = "dmr-id", .flags = SF_LIST | SF_FIELD, .prefix = TRN("My Radio"),
	.hint = TRN("OK edit  Back"),
	.count = dmr_id_count, .item = dmr_id_item, .select = dmr_id_select, .key = dmr_id_key, .enter = dmr_id_enter,
	.body = dmr_id_body,
};

#if CONFIG_REF_TRIM
// ======================= Freq trim (Settings > Advanced) =======================
// The reference trim (radio/ref_trim.h; radio-features.md 6.1), drawn as a volume-style page: the value in large type
// over a slider from -10 to +10 ppm, then what it moves a carrier by at 440 and 144 MHz. Up / Down step 0.1 ppm (0.5
// held) and are saved at once (a board that retunes for it does so once the trim holds still), so a counter or a
// hotspot shows each step; # flips the sign. Digits type the size in tenths as a calculator does (4 5 = 4.5), keeping
// the sign; OK sets it, OK with nothing typed leaves

static int32_t trim_typed;		// tenths of a ppm typed so far
static bool trim_typing, trim_neg;	// digits typed; the typed value's sign

static int32_t trim_now(void)
{
	return trim_typing ? (trim_neg ? -trim_typed : trim_typed) * 100 : ui_svc_setting_get(UI_SET_REF_TRIM);
}

static void trim_store(int32_t ppb)
{
	ppb = ref_trim_clamp(ppb);
	if (ui_svc_setting_set(UI_SET_REF_TRIM, ppb))
		ui_svc_apply(UI_SET_REF_TRIM, ppb);
	else
		ui_toast(TR("Couldn't save: try again"));
}

// "-4.5", "+1.2", "0.0": the sign always shown but on 0 (a typed minus with no digits yet shows "-0.0")
static void sb_ppm_num(sbuf_t *b, int32_t ppb, bool minus)
{
	int32_t t = (ppb < 0 ? -ppb : ppb) / 100;
	if (ppb < 0 || minus)
		sb_chr(b, '-');
	else if (ppb > 0)
		sb_chr(b, '+');
	sb_u32(b, (uint32_t)t / 10u, 1);
	sb_chr(b, '.');
	sb_u32(b, (uint32_t)t % 10u, 1);
}

#if CONFIG_UI_SETTINGS
static void sb_ppm(sbuf_t *b, int32_t ppb)
{
	sb_ppm_num(b, ppb, false);
	sb_str(b, " ppm");
}
#endif

// One line: the label at the left (dim), the value at the right
static void trim_line(const font_t *f, int y, const char *label, const char *value)
{
	int w = gfx_width();
	gfx_text(f, 2, y, label, ui_col.dim);
	gfx_text(f, w - 2 - gfx_text_width(f, value), y, value, ui_col.fg);
}

static void trim_draw(void)
{
	const ui_layout_t *L = &ui_layout;
	int w = gfx_width();
	int32_t ppb = trim_now();
	char buf[24];
	sbuf_t b;
	gfx_clear(ui_col.bg);
	int y = ui_draw_top(ui_view(), &ui_st, TR("Freq Trim"), NULL) + 2;
	bool roomy = L->hint_y - y >= L->large->h + 3 * L->medium->h + 24;	// room for the scale and the hint line

	// The value, centered: the number large, " ppm" smaller on its baseline; typing shows a cursor
	sb_init(&b, buf, sizeof buf);
	sb_ppm_num(&b, ppb, trim_typing && trim_neg && trim_typed == 0);
	if (trim_typing)
		sb_chr(&b, '_');
	const font_t *big = L->large, *unit = L->medium;
	int nw = gfx_text_width(big, buf), uw = gfx_text_width(unit, " ppm");
	int x = (w - nw - uw) / 2;
	gfx_text(big, x, y, buf, trim_typing ? ui_col.focus : ui_col.fg);
	gfx_text(unit, x + nw, y + big->h - unit->h, " ppm", ui_col.dim);
	y += big->h + (roomy ? 6 : 3);

	// The slider: a track from -max to +max, a tick at 0, the span from 0 to the value filled, a knob at the value
	int x0 = 6, x1 = w - 7, cx = (x0 + x1) / 2, half = (x1 - x0) / 2;
	int th = roomy ? 6 : 4, kh = th + (roomy ? 8 : 4), ty = y + (kh - th) / 2;
	int kx = cx + (int)((int32_t)half * ppb / REF_TRIM_MAX_PPB);
	gfx_rect(x0, ty, x1 - x0 + 1, th, ui_col.line);
	if (kx != cx)
		gfx_fill(kx < cx ? kx : cx, ty, (kx < cx ? cx - kx : kx - cx) + 1, th, ui_col.focus);
	gfx_vline(cx, y, kh, ui_col.dim);
	gfx_vline(x0, y + 1, kh - 2, ui_col.line);
	gfx_vline(x1, y + 1, kh - 2, ui_col.line);
	int kw = roomy ? 6 : 3;
	gfx_round_fill(kx - kw / 2, y, kw, kh, ui_col.fg);
	y += kh + 1;
	if (roomy) {		// the scale under it
		const font_t *t = L->small;
		gfx_text(t, x0, y, "-10", ui_col.dim);
		gfx_text(t, cx - gfx_text_width(t, "0") / 2, y, "0", ui_col.dim);
		gfx_text(t, x1 + 1 - gfx_text_width(t, "+10"), y, "+10", ui_col.dim);
		y += t->h + 4;
	}

	// What it comes to at the two bands' calling frequencies
	const font_t *f = roomy ? L->medium : L->small;
	static const uint16_t mhz[2] = { 440, 144 };
	for (int i = 0; i < 2 && y + f->h <= L->hint_y; i++) {
		int32_t hz = ref_trim_hz_at(ppb, mhz[i]);
		sb_init(&b, buf, sizeof buf);
		if (hz > 0)
			sb_chr(&b, '+');
		sb_i32(&b, hz);
		sb_str(&b, " Hz");
		// "At 440 MHz" wherever it fits beside the widest value (-4400 Hz), "@ 440 MHz" where it doesn't
		char label[20];	// "At 440 MHz", room for a longer translation
		sb_init(&b, label, sizeof label);
		const char *at = TR("At ");
		sb_str(&b, gfx_text_width(f, at) + gfx_text_width(f, "440 MHz -4400 Hz") + 4 <= w ? at : "@ ");
		sb_u32(&b, mhz[i], 1);
		sb_str(&b, " MHz");
		trim_line(f, y, label, buf);
		y += f->h;
	}
	const char *how = TR("+ to increase frequency");	// which way the sign goes, shortened only where it must be
	if (gfx_text_width(L->small, how) > w)
		how = TR("+ to increase freq");
	if (y + 2 + L->small->h <= L->hint_y && gfx_text_width(L->small, how) <= w)
		gfx_text(L->small, (w - gfx_text_width(L->small, how)) / 2, L->hint_y - L->small->h - 1, how, ui_col.dim);
	ui_draw_bottom(&ui_st, trim_typing ? TR("0-9 type  # +/-  OK set") : TR("Up Down  # +/-  OK done"));
}

static void trim_set_typed(void)
{
	trim_typing = false;
	trim_store((trim_neg ? -trim_typed : trim_typed) * 100);
}

static bool trim_key(const input_event_t *ev)
{
	bool press = ev->gesture == GESTURE_PRESS || ev->gesture == GESTURE_LONG;
	int dir = ev->action == ACT_NAV_UP || ev->action == ACT_NAV_RIGHT ? 1 :
		ev->action == ACT_NAV_DOWN || ev->action == ACT_NAV_LEFT ? -1 :
		ev->action == ACT_ENC_STEP ? (ev->value > 0 ? 1 : -1) : 0;
	if (dir != 0 && (press || ev->gesture == GESTURE_REPEAT || ev->action == ACT_ENC_STEP)) {
		if (trim_typing)
			trim_set_typed();
		// A held key steps 0.5 ppm; a knob event carries its detents (up to 8, as the other number editors)
		int n = ev->action != ACT_ENC_STEP ? 1 : ev->value < -8 || ev->value > 8 ? 8 : ev->value < 0 ? -ev->value : ev->value;
		int32_t step = ev->gesture == GESTURE_REPEAT ? 5 * REF_TRIM_STEP_PPB : n * REF_TRIM_STEP_PPB;
		trim_store(ui_svc_setting_get(UI_SET_REF_TRIM) + dir * step);
		ui_dirty();
		return true;
	}
	if (ev->action == ACT_CONTEXT && press) {		// #: the other sign
		int32_t v = ui_svc_setting_get(UI_SET_REF_TRIM);
		if (trim_typing) {
			trim_neg = !trim_neg;
		} else if (v == 0) {
			trim_typing = true;		// a minus to type after (# 4 5 OK = -4.5)
			trim_typed = 0;
			trim_neg = true;
		} else {
			trim_store(-v);
		}
		ui_dirty();
		return true;
	}
	if (ev->action >= ACT_DIGIT_0 && ev->action <= ACT_DIGIT_9 && press) {
		if (!trim_typing) {
			trim_typing = true;
			trim_typed = 0;
			trim_neg = ui_svc_setting_get(UI_SET_REF_TRIM) < 0;
		}
		int32_t v = trim_typed * 10 + (ev->action - ACT_DIGIT_0);
		if (v * 100 > REF_TRIM_MAX_PPB) {
			ui_toast(TR("Up to 10.0 ppm"));
			return true;
		}
		trim_typed = v;
		ui_dirty();
		return true;
	}
	if (ev->action == ACT_SELECT && press) {
		if (trim_typing)
			trim_set_typed();
		else
			ui_pop();
		ui_dirty();
		return true;
	}
	if (trim_typing && ev->action == ACT_BACK && ev->gesture == GESTURE_PRESS) {
		if (trim_typed > 0)
			trim_typed /= 10;	// takes the last digit back
		else
			trim_typing = false;
		ui_dirty();
		return true;
	}
	return false;
}

static void trim_enter(void)
{
	trim_typing = false;
}

static const ui_scr_def_t trim_screen = {
	.name = "freq-trim", .flags = SF_FIELD, .key = trim_key, .draw = trim_draw, .enter = trim_enter,
};
#endif

// ======================= About (Settings > Advanced) =======================
// A text page, every word shown (nothing shortened), no status strip while idle: the release and the commit, RadioFoundry's license, the
// authors, the third-party code and fonts in this image and the codec. From AUTHORS and platform/<platform>/THIRD_PARTY,
// through the header make generates (tools/gen_about.py)

static const char *const about_third[][2] = RFOUNDRY_ABOUT_THIRD_PARTY;
static uint16_t about_first;	// first line shown
static uint16_t about_max;	// the largest first line (from the last draw)

static void about_text(char *text, size_t size)
{
	sbuf_t b;
	sb_init(&b, text, size);
	// every line "Label: value" (ui_draw_text_page shows labels dim), each on its own line: "RadioFoundry v26.11" was
	// wider than the display and the release wrapped without its label. A release: its name, then the commit it was
	// built from; any other build: the commit alone
	sb_str(&b, TR("Firmware: "));
	sb_str(&b, "dm-32-m17\n");
	if (RFOUNDRY_RELEASE[0] != '\0') {
		sb_str(&b, TR("Release: "));
		sb_str(&b, RFOUNDRY_RELEASE);
		sb_chr(&b, '\n');
	}
	// Source archives can be built without Git metadata.
	if (strcmp(RFOUNDRY_GIT, "unknown") != 0) {
		sb_str(&b, TR("Build: "));
		sb_str(&b, RFOUNDRY_GIT);
		sb_chr(&b, '\n');
	}
	if (RFOUNDRY_GIT_DATE[0] != '\0') {
		sb_str(&b, TR("Date: "));
		sb_str(&b, RFOUNDRY_GIT_DATE);
		sb_chr(&b, '\n');
	}
	sb_str(&b, TR("Radio: "));
	sb_str(&b, board.name);
	sb_chr(&b, '\n');
	sb_str(&b, TR("License: "));
	sb_str(&b, "GPL-3.0-or-later\nRadioFoundry base: MIT\n(c) 2026 The RadioFoundry Authors\n");
	sb_str(&b, TR("Author:"));
	sb_str(&b, " radiosification");

	for (unsigned i = 0; i < RFOUNDRY_ABOUT_THIRD_PARTY_COUNT; i++) {
		sb_chr(&b, '\n');
		sb_str(&b, about_third[i][0]);
		sb_str(&b, ": ");
		sb_str(&b, about_third[i][1]);
	}
	sb_chr(&b, '\n');
	sb_str(&b, TR("Spleen font: BSD-2-Clause"));	// core/ui/fonts.c
#if CONFIG_FONT_AA
	sb_chr(&b, '\n');
	sb_str(&b, TR("JetBrains Mono font: OFL-1.1"));
#endif
	sb_chr(&b, '\n');
	sb_str(&b, "Codec2-mod 3200: LGPL-2.1\n"
		   "(c) 2009 David Rowe\n"
		   "(c) 2010 Perens LLC\n"
		   "Codec2 and Codec2-mod contributors\n"
		   "License: see release notices at\n"
		   "github.com/radiosification/dm32-m17");
}

static void about_enter(void)
{
	about_first = 0;
}

static void about_draw(void)
{
	char text[544];
	about_text(text, sizeof text);
	about_max = (uint16_t)ui_draw_text_page(ui_view(), &ui_st, TR("About"), text, &about_first);
}

// Up / Down and the knob scroll a line (the knob's forward = down, as in lists); BACK leaves
static bool about_key(const input_event_t *ev)
{
	int dir = ev->action == ACT_NAV_UP ? -1 : ev->action == ACT_NAV_DOWN ? 1 :
		ev->action == ACT_ENC_STEP ? (ev->value > 0 ? 1 : -1) : 0;
	if (dir == 0 || (ev->gesture != GESTURE_PRESS && ev->gesture != GESTURE_REPEAT && ev->gesture != GESTURE_LONG &&
			ev->action != ACT_ENC_STEP))
		return false;
	int n = ev->action == ACT_ENC_STEP ? (ev->value < 0 ? -ev->value : ev->value) : 1;
	int f = about_first + dir * n;
	about_first = (uint16_t)(f < 0 ? 0 : f > about_max ? about_max : f);
	ui_dirty();
	return true;
}

static const ui_scr_def_t about_screen = {
	.name = "about", .flags = SF_FIELD | SF_NO_STRIP, .key = about_key, .draw = about_draw, .enter = about_enter,
};

// ======================= Roaming (Tools > Roaming, docs/spec/dmr-roaming.md 6.2) =======================

enum { RM_ZONE, RM_NOW, RM_LOCK, RM_SURVEY, RM_BEACON, RM_LEN, RM_PROBE, RM_COUNT };



static const char *roam_body(void)
{
	static char t[120];
	sbuf_t b;
	sb_init(&b, t, sizeof t);
	sb_str(&b, TR("Roaming: not in this build"));
	return t;
}

static uint16_t roam_count(void) { return RM_COUNT; }

static void roam_item(uint16_t i, ui_item_t *it)
{
	static const char *const names[] = { TRN("Roam in this zone"), TRN("Roam now"), TRN("Lock to this site"),
					     TRN("Site survey"), TRN("Beacon interval"), TRN("Beacon length"),
					     TRN("Wake probe") };
	str_copy(it->label, sizeof it->label, tr(names[i]));
	item_num(it, (uint16_t)(i + 1u));
	it->flags |= UI_ITEM_DIM;
}

static void roam_select(uint16_t i, bool hold)
{
	(void)hold;
	(void)i;
	ui_toast(TR("Roaming: not in this build"));
}

static const ui_scr_def_t roam_screen = {
	.name = "roaming", .flags = SF_LIST | SF_NUMBERED | SF_FIELD, .prefix = TRN("Roaming"), .hint = TRN("1-7 pick  Back"),
	.count = roam_count, .item = roam_item, .select = roam_select, .body = roam_body,
};

// ======================= Crash log (Settings > Advanced, docs/spec/crash-log.md) =======================
// arg 0: from the menu (the record, the report's settings); arg 1: the prompt after the restart ("After a crash:
// Ask"). Only with the crash log; the report's rows only with the crash report

#if CONFIG_CRASH_SCREEN
enum { CR_SEND, CR_TO, CR_MODE, CR_CLEAR, CR_LATER };

static bool crash_prompt(void) { return ui_frame()->arg == 1; }

static uint8_t crash_rows(uint8_t *out)
{
	uint8_t n = 0;
	if (CONFIG_CRASH_REPORT)
		out[n++] = CR_SEND;
	if (crash_prompt()) {
		out[n++] = CR_LATER;
		return n;
	}
	if (CONFIG_CRASH_REPORT) {
		out[n++] = CR_TO;
		out[n++] = CR_MODE;
	}
	out[n++] = CR_CLEAR;
	return n;
}

static uint16_t crash_count(void)
{
	uint8_t r[5];
	return crash_rows(r);
}

// The target contact's name (or "TG 9" / its ID without one); "" when none is set or it is gone from the codeplug
static void crash_target(char *out, size_t len)
{
	sbuf_t b;
	sb_init(&b, out, len);
}

static const char *const crash_modes[] = { TRN("Off"), TRN("Ask"), TRN("Auto") };	// UI_CRASH_SEND_*

static const char *crash_body(void)
{
	static char t[160];
	sbuf_t b;
	sb_init(&b, t, sizeof t);
	ui_crash_t c;
	if (!ui_svc_crash_get(&c)) {
		sb_str(&b, TR("No crash recorded."));
		return t;
	}
	if (crash_prompt()) {
		// The question only: it has to fit above the two rows on the smallest screens
		char to[CODEPLUG_NAME_LEN + 4];
		crash_target(to, sizeof to);
		sb_str(&b, TR("Restarted after a crash. Send the report to "));
		sb_str(&b, to);
		sb_str(&b, "?");
		return t;
	}
	sb_str(&b, c.summary);
	if (c.sent) {
		sb_chr(&b, '\n');
		sb_str(&b, TR("Report sent"));
	}
	if (c.count > 1) {
		sb_str(&b, "\n");
		sb_u32(&b, c.count, 1);
		sb_str(&b, TR(" crashes in a row"));
	}
	sb_chr(&b, '\n');
	sb_str(&b, TR("Ran for "));
	sb_mmss(&b, c.uptime_ms / 1000u);
	if (c.build != 0) {
		sb_str(&b, TR(", build "));
		for (int k = 28; k >= 0; k -= 4)
			sb_chr(&b, "0123456789abcdef"[(c.build >> k) & 0xFu]);
	}
	return t;
}

static void crash_item(uint16_t i, ui_item_t *it)
{
	uint8_t r[5];
	uint8_t n = crash_rows(r);
	if (i >= n)
		return;
	ui_crash_t c;
	bool have = ui_svc_crash_get(&c);
	item_num(it, (uint16_t)(i + 1u));
	switch (r[i]) {
	case CR_SEND:
		str_copy(it->label, sizeof it->label, TR("Send report"));
		if (!have)
			it->flags |= UI_ITEM_DIM;
		break;
	case CR_TO:
		str_copy(it->label, sizeof it->label, TR("Send to"));
		crash_target(it->value, sizeof it->value);
		if (it->value[0] == 0)
			str_copy(it->value, sizeof it->value, TR("Not set"));
		break;
	case CR_MODE: {
		int32_t m = ui_svc_setting_get(UI_SET_CRASH_MODE);
		str_copy(it->label, sizeof it->label, TR("After a crash"));
		str_copy(it->value, sizeof it->value, tr(crash_modes[m >= 0 && m <= UI_CRASH_SEND_AUTO ? m : 0]));
		break;
	}
	case CR_CLEAR:
		str_copy(it->label, sizeof it->label, TR("Clear"));
		if (!have)
			it->flags |= UI_ITEM_DIM;
		break;
	default:
		str_copy(it->label, sizeof it->label, TR("Not now"));
		break;
	}
}

static bool crash_watch;	// a report of ours is going: say how it ended

static void crash_select(uint16_t i, bool hold)
{
	(void)hold;
	uint8_t r[5];
	uint8_t n = crash_rows(r);
	if (i >= n)
		return;
	ui_crash_t c;
	bool have = ui_svc_crash_get(&c);
	char to[CODEPLUG_NAME_LEN + 4];
	switch (r[i]) {
	case CR_SEND:
		crash_target(to, sizeof to);
		if (!have) {
			ui_toast(TR("No crash recorded"));
		} else if (to[0] == 0) {
			ui_toast(TR("Pick Send to first"));
		} else if (ui_svc_crash_send()) {
			crash_watch = true;
			ui_toast(TR("Sending crash report"));
			if (crash_prompt())
				ui_pop();
		} else {
			ui_toast(TR("Can't send now: try again"));
		}
		break;
	case CR_TO:
		if (CONFIG_DMR)
			ui_push(SCR_CRASH_TO, 0);
		break;
	case CR_MODE: {
		int32_t m = ui_svc_setting_get(UI_SET_CRASH_MODE);
		int32_t nm = m >= UI_CRASH_SEND_OFF && m < UI_CRASH_SEND_AUTO ? m + 1 : UI_CRASH_SEND_OFF;
		if (!ui_svc_setting_set(UI_SET_CRASH_MODE, nm))
			ui_toast(TR("Couldn't save: try again"));
		else if (nm != UI_CRASH_SEND_OFF && ui_svc_setting_get(UI_SET_CRASH_CONTACT) <= 0)
			ui_toast(TR("Pick Send to as well"));
		break;
	}
	case CR_CLEAR:
		if (have) {
			ui_svc_crash_clear();
			ui_toast(TR("Crash record cleared"));
		}
		break;
	default:
		ui_pop();
		break;
	}
}

static void crash_enter(void)
{
	ui_svc_crash_seen();	// shown: no prompt for it again, and the next crash counts from 1
}

static const ui_scr_def_t crash_screen = {
	.name = "crash", .flags = SF_LIST | SF_NUMBERED, .prefix = TRN("Crash Log"), .hint = TRN("OK pick  Back"),
	.count = crash_count, .item = crash_item, .select = crash_select, .body = crash_body, .enter = crash_enter,
};


// After the restart (ui_app_tick): once the Home screen has been up a moment, a record nobody has seen is asked about
// (Ask) or sent (Auto); Off leaves it to the menu. Then the report's end, once, as a toast
#define CRASH_BOOT_MS	1500u

static uint32_t crash_since;
static bool crash_checked;

void ui_crash_reset(void)
{
	crash_since = 0;
	crash_checked = false;
	crash_watch = false;
}

void ui_crash_tick(uint32_t now_ms)
{
	if (!crash_checked) {
		if (crash_since == 0)
			crash_since = now_ms != 0 ? now_ms : 1;
		if (now_ms - crash_since < CRASH_BOOT_MS || ui_depth() != 0)
			return;
		crash_checked = true;
		ui_crash_t c;
		char to[CODEPLUG_NAME_LEN + 4];
		int32_t mode = CONFIG_CRASH_REPORT ? ui_svc_setting_get(UI_SET_CRASH_MODE) : UI_CRASH_SEND_OFF;
		if (!ui_svc_crash_get(&c) || c.seen || c.sent || mode == UI_CRASH_SEND_OFF)
			return;
		crash_target(to, sizeof to);
		if (to[0] == 0)
			return;		// nowhere to send it: the menu shows it
		if (mode == UI_CRASH_SEND_ASK) {
			ui_push(SCR_CRASH, 1);
		} else if (ui_svc_crash_send()) {
			ui_svc_crash_seen();
			crash_watch = true;
			ui_toast(TR("Sending crash report"));
		}
		return;
	}
	if (!crash_watch)
		return;
	uint8_t st = ui_svc_crash_tx_state();
	if (st == UI_CRASH_TX_OK || st == UI_CRASH_TX_FAILED) {
		crash_watch = false;
		ui_toast(st == UI_CRASH_TX_OK ? TR("Crash report sent") : TR("Crash report not sent"));
	}
}
#endif

// ======================= Tools =======================

// Every tool keeps its number on every radio and build, so a key path (OK 6 7: Diagnostics; tests/hil/test_ui.py and the
// user guide) is the same everywhere. The one a radio may not have (Battery) is last, so no radio has a gap; a build
// without diagnostics skips 7
enum { T_SCAN, T_POSITION, T_HOTSPOT, T_USB_NET, T_SFR, T_ROAM, T_DIAG, T_APRS, T_BATTERY, T_ZONE_OFFER, T_COUNT };

// A tool this build or radio doesn't have is left out of the list (Battery, Supply on a radio that measures its DC
// input instead, is left out where the radio measures neither)
static bool tool_shown(uint16_t i)
{
	return (i != T_DIAG || CONFIG_DIAG) && (i != T_BATTERY || board_volts() != BOARD_VOLTS_NONE) &&
	       (i != T_HOTSPOT || CONFIG_HOTSPOT) && (i != T_USB_NET || USB_NET_UI) && (i != T_SFR || CONFIG_SFR) &&
	       i != T_ZONE_OFFER;
}

// The hint names the last number ("1-9 open"): the highest tool this radio has. Built in the language in use on each
// count from the keys and the translated rest (the list draws its hint through tr(), which leaves text it doesn't know
// as it is)
static char tools_hint[24] = "1-9 open  Back";

static uint16_t tools_count(void)
{
	uint16_t n = 0, last = 0;
	for (uint16_t i = 0; i < T_COUNT; i++) {
		if (tool_shown(i)) {
			n++;
			last = (uint16_t)(i + 1u);
		}
	}
	sbuf_t b;
	sb_init(&b, tools_hint, sizeof tools_hint);
	sb_str(&b, "1-");
	sb_chr(&b, (char)('0' + (last < 9 ? last : 9)));	// "1-9": the zone offer's 0 isn't named
	sb_str(&b, TR(" open  Back"));
	return n;
}

// The tool on list row `row`
static uint16_t tools_row(uint16_t row)
{
	for (uint16_t i = 0; i < T_COUNT; i++) {
		if (tool_shown(i) && row-- == 0)
			return i;
	}
	return T_COUNT;
}

static void tools_item(uint16_t row, ui_item_t *it)
{
	static const char *const names[] = { TRN("Scan"), TRN("Position"), TRN("Hotspot"), TRN("USB network"),
					     TRN("Single-freq repeater"), TRN("Roaming"), TRN("Diagnostics"), "APRS",
					     TRN("Battery"), TRN("Zones/Chs Received") };
	uint16_t i = tools_row(row);
	if (i >= T_COUNT)
		return;
	str_copy(it->label, sizeof it->label, i == T_BATTERY && board_volts() == BOARD_VOLTS_SUPPLY ? TR("Supply") :
		 tr(names[i]));
	item_num(it, (uint16_t)(i + 1u));	// the tool's own number, not its row (see the enum)
	sbuf_t b;
	sb_init(&b, it->value, sizeof it->value);
	switch (i) {
	case T_SCAN:
#if CONFIG_DUAL_WATCH
		if (ui_st.dual.on) {
			sb_str(&b, TR("Dual watch"));
			break;
		}
#endif
		sb_str(&b, ui_st.scanning ? TR("On") : TR("Off"));
		if (!(feats() & UI_FEAT_SCAN))
			it->flags |= UI_ITEM_DIM;
		break;
	case T_POSITION:
		if (!(feats() & UI_FEAT_GPS))
			it->flags |= UI_ITEM_DIM;
		break;
	case T_HOTSPOT:
		it->flags |= UI_ITEM_DIM;
		break;
	case T_USB_NET:
#if USB_NET_UI
		sb_str(&b, hostlink_network_mode() ? TR("Network") : TR("Serial"));
#else
		it->flags |= UI_ITEM_DIM;
#endif
		break;
	case T_SFR:
		if (!CONFIG_SFR)
			it->flags |= UI_ITEM_DIM;
		break;
	case T_BATTERY:
		if (board_volts() == BOARD_VOLTS_SUPPLY) {
			sb_volts(&b, ui_view()->supply_mv);
			break;
		}
		sb_u32(&b, ui_st.battery_pct, 1);
		sb_chr(&b, '%');
		break;
	case T_ROAM:
		it->flags |= UI_ITEM_DIM;
		break;
	case T_APRS:
		if (!CONFIG_APRS)
			it->flags |= UI_ITEM_DIM;
		break;
	default:
		break;
	}
}

static void tools_select(uint16_t row, bool hold)
{
	(void)hold;
	switch (tools_row(row)) {
	case T_SCAN:
		if (!(feats() & UI_FEAT_SCAN)) {
			ui_toast(TR("Scan: not in this build"));
		} else {
			ui_go_home();
			ui_scan_toggle();	// says what scans: the zone, or the band in the VFO (dual watch ends)
		}
		break;
	case T_POSITION: ui_push(CONFIG_GPS_SKY ? SCR_GPS : SCR_INFO, INFO_POSITION); break;
	case T_HOTSPOT: ui_push(SCR_INFO, INFO_HOTSPOT); break;
	case T_USB_NET: ui_push(SCR_INFO, INFO_USB_NET); break;
	case T_SFR: ui_push(SCR_INFO, INFO_SFR); break;
	case T_BATTERY: ui_push(SCR_INFO, INFO_BATTERY); break;
	case T_DIAG: ui_push(SCR_DIAG, 0); break;
	case T_ROAM: ui_push(SCR_ROAM, 0); break;
	case T_APRS: ui_push(SCR_APRS, 0); break;
	case T_ZONE_OFFER: ui_push(SCR_OFFER, 0); break;
	default: break;
	}
}

static const ui_scr_def_t tools_screen = {
	.name = "tools", .flags = SF_LIST | SF_NUMBERED | SF_FIELD, .prefix = TRN("Tools"),
	.hint = tools_hint,
	.count = tools_count, .item = tools_item, .select = tools_select,
};

// ======================= Settings by intent (information-architecture.md 6) =======================
// CONFIG_UI_SETTINGS=n leaves the whole menu out (the settings keep their stored values; qdmr sets them)
#if CONFIG_UI_SETTINGS

typedef enum { ST_TOGGLE, ST_LIST, ST_NUMBER, ST_ACTION } set_type_t;
enum { SA_DIAG, SA_FIRMWARE, SA_CODEPLUG, SA_RESET, SA_MY_RADIO, SA_CRASH, SA_LANGUAGE };

typedef struct {
	const char *name;
	const char *syn;		// another name Find knows it by
	uint8_t type;			// set_type_t
	uint8_t id;			// ui_setting_t, or SA_* for an action
	int16_t min, max, step;		// ST_NUMBER
	const int16_t *values;		// ST_LIST: the values ...
	const char *const *names;	// ... and their names (NULL: number + unit)
	uint8_t n;
	const char *unit;
} set_item_t;

typedef struct {
	const char *name;
	const char *title;	// the group's page title, in Title Case (the name is its row in Settings)
	const set_item_t *items;
	uint8_t count;
} set_group_t;

static const int16_t v_backlight[] = { 5, 10, 30, 60, 120, 0 };
// Dim after / off after (four times as long): both times shown, so that the dim step isn't taken for the time-out
static const char *const n_backlight[] = { "5s/20s", "10s/40s", "30s/2m", "1m/4m", "2m/8m", TRN("Never") };
#if CONFIG_UI_COLOR
static const int16_t v_theme[] = { 0, 1, 2 };
static const char *const n_theme[] = { TRN("Dark"), TRN("Pink"), TRN("High contrast") };
static const int16_t v_accent[] = { UI_ACCENT_DEFAULT, UI_ACCENT_RED, UI_ACCENT_ORANGE, UI_ACCENT_YELLOW, UI_ACCENT_TEAL,
				     UI_ACCENT_BLUE, UI_ACCENT_PURPLE, UI_ACCENT_PINK };
static const char *const n_accent[] = { TRN("Default"), TRN("Red"), TRN("Orange"), TRN("Yellow"), TRN("Teal"),
					TRN("Light blue"), TRN("Purple"), TRN("Pink") };
#endif
static const int16_t v_tot[] = { 0, 30, 60, 120, 180, 300, 600 };
static const char *const n_tot[] = { TRN("Off"), "30 s", TRN("1 min"), TRN("2 min"), TRN("3 min"), TRN("5 min"),
				     TRN("10 min") };
static const int16_t v_lock[] = { 0, 30, 60, 300 };
static const char *const n_lock[] = { TRN("Off"), "30 s", TRN("1 min"), TRN("5 min") };
static const int16_t v_posfmt[] = { 0, 1, 2 };
static const char *const n_posfmt[] = { TRN("Degrees"), TRN("Deg min sec"), TRN("Grid") };
static const int16_t v_possend[] = { 0, 60, 300, 900 };
static const char *const n_possend[] = { TRN("Off"), TRN("1 min"), TRN("5 min"), TRN("15 min") };
#if CONFIG_KEY_ASSIGN
// Side keys (core/input/keyassign.h, KEYFN_* values): monitor needs the key held, so it is a press only;
// emergency is hold to activate, so a long press only
static const int16_t v_keyfn_press[] = { KEYFN_DEFAULT, KEYFN_NONE, KEYFN_MONITOR, KEYFN_TALK_TARGET, KEYFN_POWER,
					 KEYFN_ZONE_UP, KEYFN_ZONE_DOWN, KEYFN_SCAN, KEYFN_KEY_LOCK,
					 KEYFN_HOME_ZONE,
#if CONFIG_VFO_FM || CONFIG_VFO_DMR
					 KEYFN_COPY_VFO,
#endif
#if CONFIG_DUAL_WATCH
					 KEYFN_AB, KEYFN_DUAL_WATCH,
#endif
};
static const char *const n_keyfn_press[] = { TRN("Default"), TRN("Nothing"), TRN("Monitor"), TRN("Talk target"),
					     TRN("Power level"), TRN("Next zone"), TRN("Previous zone"), TRN("Scan"),
					     TRN("Key lock"), TRN("Home zone"),
#if CONFIG_VFO_FM || CONFIG_VFO_DMR
					     TRN("Copy to VFO"),
#endif
#if CONFIG_DUAL_WATCH
					     "A/B", TRN("Dual watch"),
#endif
};
static const int16_t v_keyfn_hold[] = { KEYFN_DEFAULT, KEYFN_NONE, KEYFN_TALK_TARGET, KEYFN_POWER, KEYFN_ZONE_UP,
					KEYFN_ZONE_DOWN, KEYFN_SCAN,
					KEYFN_KEY_LOCK, KEYFN_HOME_ZONE,
#if CONFIG_VFO_FM || CONFIG_VFO_DMR
					KEYFN_COPY_VFO,
#endif
#if CONFIG_DUAL_WATCH
					KEYFN_AB, KEYFN_DUAL_WATCH,
#endif
};
static const char *const n_keyfn_hold[] = { TRN("Default"), TRN("Nothing"), TRN("Talk target"), TRN("Power level"),
					    TRN("Next zone"), TRN("Previous zone"), TRN("Scan"),
					    TRN("Key lock"), TRN("Home zone"),
#if CONFIG_VFO_FM || CONFIG_VFO_DMR
					    TRN("Copy to VFO"),
#endif
#if CONFIG_DUAL_WATCH
					    "A/B", TRN("Dual watch"),
#endif
};
#endif
static const int16_t v_updown[] = { 0, 1, 2 };
static const char *const n_updown[] = { TRN("Channel"), TRN("Zone"), TRN("Talk target") };
#if CONFIG_VFO
static const int16_t v_step[] = { 0, 250, 500, 625, 833, 1000, 1250, 2000, 2500, 5000, 10000 };	// 10 Hz units
static const char *const n_step[] = { TRN("Auto"), "2.5 kHz", "5 kHz", "6.25 kHz", "8.33 kHz", "10 kHz", "12.5 kHz",
				      "20 kHz", "25 kHz", "50 kHz", "100 kHz" };
#endif
#if CONFIG_DUAL_WATCH
static const int16_t v_dual_wait[] = { 500, 1000, 1500, 2000, 3000, 5000 };	// ms
static const char *const n_dual_wait[] = { "0.5 s", "1 s", "1.5 s", "2 s", "3 s", "5 s" };
#endif
static const int16_t v_autooff[] = { 0, 30, 60, 120 };
static const char *const n_autooff[] = { TRN("Off"), TRN("30 min"), "1 h", "2 h" };

#define TOGGLE(nm, syn_, sid) { nm, syn_, ST_TOGGLE, sid, 0, 1, 1, NULL, NULL, 0, NULL }
#define NUMBER(nm, syn_, sid, lo, hi, st, u) { nm, syn_, ST_NUMBER, sid, lo, hi, st, NULL, NULL, 0, u }
#define LIST(nm, syn_, sid, v, n) { nm, syn_, ST_LIST, sid, 0, 0, 0, v, n, sizeof v / sizeof v[0], NULL }
#define ACTION(nm, syn_, a) { nm, syn_, ST_ACTION, a, 0, 0, 0, NULL, NULL, 0, NULL }

static const set_item_t s_sound[] = {
	NUMBER(TRN("Volume"), "speaker loudness", UI_SET_VOLUME, 0, RADIO_VOLUME_MAX, 1, NULL),
	TOGGLE(TRN("Key tones"), "beep", UI_SET_KEY_TONES),
	NUMBER(TRN("Tone volume"), NULL, UI_SET_TONE_VOLUME, 0, 10, 1, NULL),
};
static const set_item_t s_display[] = {
	NUMBER(TRN("Brightness"), "backlight", UI_SET_BRIGHTNESS, 10, 100, 10, "%"),
	LIST(TRN("Backlight Dim/Off"), "timeout", UI_SET_BACKLIGHT_S, v_backlight, n_backlight),
#if CONFIG_UI_COLOR	// a radio whose only screen is monochrome has no colors to set: no rows for them
	LIST(TRN("Theme"), "contrast", UI_SET_THEME, v_theme, n_theme),
	LIST(TRN("Highlight color"), "accent", UI_SET_ACCENT, v_accent, n_accent),
	TOGGLE(TRN("Filled status bar"), "top line", UI_SET_STRIP),
#endif
	TOGGLE(TRN("Large text"), "accessibility", UI_SET_LARGE_TEXT),
	TOGGLE(TRN("Hint row"), "help", UI_SET_HINTS),
	// Last, so the rows above keep their places; its search words are "language" in other languages, for a user who
	// can't read the English
	ACTION(TRN("Language"), "idioma langue sprache lingua taal jezyk", SA_LANGUAGE),
};
static const set_item_t s_talk[] = {
	NUMBER(TRN("Reply window"), NULL, UI_SET_REPLY_S, 0, 30, 1, " s"),
	LIST(TRN("TX timeout"), "TOT", UI_SET_TOT_S, v_tot, n_tot),
	NUMBER(TRN("Default squelch"), NULL, UI_SET_SQUELCH, 0, 9, 1, NULL),
#if CONFIG_VFO
	LIST(TRN("VFO step"), "frequency tuning channel spacing", UI_SET_VFO_STEP, v_step, n_step),
#endif
	NUMBER(TRN("Mic gain"), NULL, UI_SET_MIC_GAIN, 0, 10, 1, NULL),
	TOGGLE(TRN("M17 mic AGC"), "automatic gain level", UI_SET_MIC_AGC),
#if CONFIG_DUAL_WATCH
	TOGGLE(TRN("Dual watch talk back"), "reply other channel A B", UI_SET_DUAL_TALK_BACK),
	LIST(TRN("Dual watch delay"), "resume wait reply A B", UI_SET_DUAL_DELAY_MS, v_dual_wait, n_dual_wait),
#endif
};
static const set_item_t s_key_lock[] = {
	LIST(TRN("Auto lock"), "keylock", UI_SET_AUTO_LOCK_S, v_lock, n_lock),
	TOGGLE(TRN("PTT when locked"), NULL, UI_SET_PTT_LOCKED),
};
static const set_item_t s_location[] = {
	TOGGLE("GPS", NULL, UI_SET_GPS),
	LIST(TRN("Position format"), NULL, UI_SET_POS_FORMAT, v_posfmt, n_posfmt),
	LIST(TRN("Send position"), "APRS", UI_SET_POS_SEND_S, v_possend, n_possend),
};
static const set_item_t s_power[] = {
	TOGGLE(TRN("Battery saver"), NULL, UI_SET_BATTERY_SAVER),
	LIST(TRN("Auto power off"), NULL, UI_SET_AUTO_OFF_MIN, v_autooff, n_autooff),
	NUMBER(TRN("Low battery warning"), NULL, UI_SET_LOW_BATTERY, 5, 30, 5, "%"),
};
#if CONFIG_KEY_ASSIGN
// Settings > Keys: a row pair per key the board lets the user reassign, named as on the radio ("SK1", "SK1 hold")
_Static_assert(UI_KEY_ROLES == 9, "KEY_ROWS below");
static char key_label[2 * UI_KEY_ROLES][20];
static uint8_t key_label_lang = 0xFF;	// the language they were built in: rebuilt when the user picks another

static void key_labels(void)
{
	sbuf_t b;

	if (key_label_lang == lang_current())
		return;
	key_label_lang = lang_current();
	for (uint8_t r = 0; r < UI_KEY_ROLES; r++) {
		for (uint8_t h = 0; h < 2; h++) {
			sb_init(&b, key_label[2 * r + h], sizeof key_label[0]);
			if (board.side_key_names != NULL && r < board.side_key_count) {
				sb_str(&b, board.side_key_names[r]);
			} else {
				sb_str(&b, TR("Key "));
				sb_i32(&b, r + 1);
			}
			if (h)
				sb_str(&b, TR(" hold"));
		}
	}
}
#endif
static const set_item_t s_keys[] = {
	NUMBER(TRN("Long press time"), NULL, UI_SET_LONG_PRESS_MS, 400, 1200, 100, " ms"),
	NUMBER(TRN("Key repeat"), NULL, UI_SET_REPEAT_MS, 60, 300, 20, " ms"),
	LIST(TRN("Up/Down on Home"), NULL, UI_SET_UPDOWN_HOME, v_updown, n_updown),
#if CONFIG_KEY_ASSIGN
#define KEY_ROWS(r) \
	LIST(key_label[2 * (r)], "button programmable", UI_SET_KEY_FIRST + 2 * (r), v_keyfn_press, n_keyfn_press), \
	LIST(key_label[2 * (r) + 1], "button programmable long press", UI_SET_KEY_FIRST + 2 * (r) + 1, v_keyfn_hold, \
	     n_keyfn_hold)
	KEY_ROWS(0), KEY_ROWS(1), KEY_ROWS(2), KEY_ROWS(3), KEY_ROWS(4), KEY_ROWS(5), KEY_ROWS(6), KEY_ROWS(7),
	KEY_ROWS(8),
#endif
};
static const set_item_t s_my_radio[] = {
	ACTION(TRN("Radio name / M17 callsign"), "callsign", SA_MY_RADIO),
	// Also in Advanced, where users didn't look for it
	ACTION(TRN("About"), "firmware version release build date", SA_FIRMWARE),
};
static const set_item_t s_advanced[] = {
	ACTION(TRN("About"), "firmware version license authors", SA_FIRMWARE),
	ACTION(TRN("Codeplug"), NULL, SA_CODEPLUG),
	ACTION(TRN("Diagnostics"), "debug", SA_DIAG),
	ACTION(TRN("Factory reset"), "erase", SA_RESET),
	// After Factory reset, so its place (and its tests' key presses) stays
#if CONFIG_CRASH_SCREEN
	ACTION(TRN("Crash log"), "crash report fault debug", SA_CRASH),
#endif
#if CONFIG_REF_TRIM
	NUMBER(TRN("Freq trim"), "frequency offset reference oscillator calibration ppm", UI_SET_REF_TRIM, -REF_TRIM_MAX_PPB,
	       REF_TRIM_MAX_PPB, REF_TRIM_STEP_PPB, NULL),	// its own page (trim_screen); last: Up from the top
#endif
};

#define GROUP(nm, a)		{ nm, nm, a, sizeof a / sizeof a[0] }
#define GROUP_T(nm, t, a)	{ nm, t, a, sizeof a / sizeof a[0] }
static const set_group_t groups[] = {
	GROUP(TRN("Sound"), s_sound), GROUP(TRN("Display"), s_display),
	GROUP_T(TRN("Talk & listen"), TRN("Talk & Listen"), s_talk),
	GROUP(TRN("Location"), s_location), GROUP(TRN("Power"), s_power), GROUP(TRN("Key lock"), s_key_lock),
	GROUP(TRN("Keys"), s_keys), GROUP_T(TRN("My radio"), TRN("My Radio"), s_my_radio), GROUP(TRN("Advanced"), s_advanced),
};
#define GROUPS	(sizeof groups / sizeof groups[0])

uint8_t ui_set_group_count(void) { return GROUPS; }
// In English, as Find matches the name in the language in use and in English (it calls tr() on it)
const char *ui_set_group_name(uint8_t g) { return g < GROUPS ? groups[g].name : ""; }

// Settings a radio without a battery (or a broadcast receiver, or the key) doesn't have are left out of the list, not
// shown disabled
static bool set_hidden(const set_item_t *s)
{
#if CONFIG_KEY_ASSIGN
	if (s->type != ST_ACTION && s->id >= UI_SET_KEY_FIRST && s->id <= UI_SET_KEY_LAST) {
		key_labels();
		return (s->id - UI_SET_KEY_FIRST) / 2 >= board.side_key_count;	// a key this radio doesn't have
	}
#endif
	if (s->type != ST_ACTION && s->id == UI_SET_VOLUME)
		return !(feats() & UI_FEAT_VOLUME);	// a knob sets the volume on the other radios
	if (s->type != ST_ACTION && s->id == UI_SET_BCAST_STEP)
		return !(feats() & UI_FEAT_BCAST);
	if (s->type != ST_ACTION && s->id == UI_SET_REF_TRIM)
		return !(feats() & UI_FEAT_REF_TRIM);	// only a radio whose board has the trim
	return !board_has_battery() && s->type != ST_ACTION &&
	       (s->id == UI_SET_BATTERY_SAVER || s->id == UI_SET_LOW_BATTERY);
}

// The i-th setting of group g that this radio has, NULL past the end
static const set_item_t *set_nth(uint8_t g, uint16_t i)
{
	if (g >= GROUPS)
		return NULL;
	for (uint8_t k = 0; k < groups[g].count; k++) {
		if (set_hidden(&groups[g].items[k]))
			continue;
		if (i-- == 0)
			return &groups[g].items[k];
	}
	return NULL;
}

uint8_t ui_set_item_count(uint8_t g)
{
	uint8_t n = 0;
	while (set_nth(g, n) != NULL)
		n++;
	return n;
}

// In English, as ui_set_group_name (a key's row is already in the language in use: tr() leaves it as it is)
const char *ui_set_item_name(uint8_t g, uint8_t i, const char **synonyms)
{
	const set_item_t *s = set_nth(g, i);
	*synonyms = s != NULL ? s->syn : NULL;
	return s != NULL ? s->name : "";
}

void ui_open_setting(uint8_t g, uint8_t i)
{
	ui_push(SCR_SETTINGS, 0);
	ui_frame()->focus = g;
	ui_push(SCR_GROUP, g);
	ui_frame()->focus = i;
}

static uint16_t settings_count(void) { return GROUPS; }

static void settings_item(uint16_t i, ui_item_t *it)
{
	str_copy(it->label, sizeof it->label, tr(groups[i].name));
	item_num(it, (uint16_t)(i + 1u));
}

static void settings_select(uint16_t i, bool hold)
{
	(void)hold;
	ui_push(SCR_GROUP, i);
}

static const ui_scr_def_t settings_screen = {
	.name = "settings", .flags = SF_LIST | SF_NUMBERED, .prefix = TRN("Settings"), .hint = TRN("0-9 open  Back"),
	.count = settings_count, .item = settings_item, .select = settings_select,
};

// A group: each row shows its value; toggles and lists change on OK, numbers open an inline editor

static bool set_editing;
static bool set_typed;		// digits typed into the editor since it opened
static int32_t set_value;

static const set_item_t *set_item_at(uint16_t i)
{
	uint16_t g = ui_frame()->arg;
	return g < GROUPS ? set_nth((uint8_t)g, i) : NULL;
}

static void set_value_text(const set_item_t *s, int32_t v, char *out, size_t len)
{
	sbuf_t b;
	sb_init(&b, out, len);
	switch (s->type) {
	case ST_TOGGLE:
		sb_str(&b, v ? TR("On") : TR("Off"));
		break;
	case ST_LIST:
		for (uint8_t k = 0; k < s->n; k++) {
			if (s->values[k] == v) {
				sb_str(&b, tr(s->names[k]));
				return;
			}
		}
		sb_i32(&b, v);
		break;
	case ST_NUMBER:
#if CONFIG_REF_TRIM
		if (s->id == UI_SET_REF_TRIM) {
			sb_ppm(&b, v);
			break;
		}
#endif
		sb_i32(&b, v);
		if (s->unit != NULL)
			sb_str(&b, s->unit);
		break;
	default:
		break;
	}
}

static uint16_t group_count(void) { return ui_set_item_count((uint8_t)ui_frame()->arg); }


// The board's vocoder takes this encode option (hal_vocoder_t.options)
static bool vocoder_has(uint32_t opt)
{
	return board.vocoder != NULL && board.vocoder->options != NULL && (board.vocoder->options() & opt) != 0;
}

// Mic AGC is on and in force: Mic gain doesn't apply (shown as "AGC", dimmed)
static bool mic_agc_on(void)
{
	return vocoder_has(VOCODER_OPT_AGC) && ui_svc_setting_get(UI_SET_MIC_AGC) != 0;
}

// Why a setting can't be used on this radio or in this build (the row is dimmed and OK says it), NULL if it can
static const char *set_why_not(const set_item_t *s)
{
	if (s->type == ST_ACTION)
		return NULL;
	switch (s->id) {
	case UI_SET_VOICE_PROMPTS:
		return TR("Voice prompts: not in this build");
	case UI_SET_TALK_PERMIT:
		return CONFIG_DMR && CONFIG_BEEP ? NULL : TR("Talk permit tone: not in this build");
	case UI_SET_MESSAGE_TONE:
		return feats() & UI_FEAT_SMS ? NULL : TR("Messages: not in this build");
	case UI_SET_VIBRATE:
		return feats() & UI_FEAT_VIBRATE ? NULL : TR("This radio has no vibration motor");
	case UI_SET_THEME:
	case UI_SET_ACCENT:
	case UI_SET_STRIP:
		return board.display != NULL && board.display->bpp == 1 ? TR("Colors are for color screens") : NULL;
	case UI_SET_MIC_GAIN:
		if (!mic_gain_available())
			return TR("Mic gain: not in this build");
		return mic_agc_on() ? TR("Mic AGC sets the level") : NULL;
	case UI_SET_MIC_AGC:
		if(CONFIG_M17)return NULL;
		if (!CONFIG_DMR)
			return TR("Mic AGC: not in this build");
		return vocoder_has(VOCODER_OPT_AGC) ? NULL : TR("Not on this radio");
	case UI_SET_HEAR_ALL_DEFAULT:
		return feats() & UI_FEAT_HEAR_ALL ? NULL : TR("Digital monitor: not in this build");
	case UI_SET_ALIAS_SHOW:
		return CONFIG_DMR_TALKER_ALIAS ? NULL : TR("Talker alias: not in this build");
	case UI_SET_PRIVACY_TYPE:
	case UI_SET_PRIVACY_KEY:
		if (!(feats() & UI_FEAT_PRIVACY))
			return TR("Privacy: not in this build");
		return s->id == UI_SET_PRIVACY_KEY && ui_svc_setting_get(UI_SET_PRIVACY_TYPE) == 0 ?
			TR("Pick a key type first") : NULL;
	case UI_SET_GPS:
	case UI_SET_POS_FORMAT:
	case UI_SET_POS_SEND_S:
		return board_has_gps() ? NULL : TR("No GPS on this radio");
	default:
		return NULL;
	}
}

// A number setting's range: the privacy key's follows its type (Motorola 1-255, Anytone 0-65535)
static void set_range(const set_item_t *s, int32_t *lo, int32_t *hi)
{
	*lo = s->min;
	*hi = s->max;
	if (s->id == UI_SET_PRIVACY_KEY && ui_svc_setting_get(UI_SET_PRIVACY_TYPE) == 2) {
		*lo = 0;
		*hi = 65535;
	}

}

static void group_item(uint16_t i, ui_item_t *it)
{
	const set_item_t *s = set_item_at(i);
	if (s == NULL)
		return;
	str_copy(it->label, sizeof it->label, tr(s->name));
	bool edit = set_editing && i == ui_frame()->focus;
	if (s->type != ST_ACTION)
		set_value_text(s, edit ? set_value : ui_svc_setting_get(s->id), it->value, sizeof it->value);
	else if (s->id == SA_LANGUAGE)
		str_copy(it->value, sizeof it->value, lang_name(lang_current()));
	if (s->id == UI_SET_PRIVACY_KEY && s->type == ST_NUMBER && !edit && ui_svc_setting_get(UI_SET_PRIVACY_TYPE) == 0)
		strcpy(it->value, "-");
	if (s->id == UI_SET_MIC_GAIN && !edit && mic_agc_on())
		strcpy(it->value, "AGC");
	if (edit)
		it->flags |= UI_ITEM_EDIT;
	if (set_why_not(s) != NULL)
		it->flags |= UI_ITEM_DIM;
}

static void set_store(const set_item_t *s, int32_t v)
{
	if (ui_svc_setting_set(s->id, v)) {
		ui_apply_setting(s->id, v);	// the ones the UI acts on itself
		ui_svc_apply(s->id, v);		// and the ones other packages do
	} else {
		ui_toast(TR("Couldn't save: try again"));
	}
}

static void group_select(uint16_t i, bool hold)
{
	const set_item_t *s = set_item_at(i);
	if (s == NULL)
		return;
	const char *why = set_why_not(s);
	if (why != NULL) {
		set_editing = false;
		ui_toast(why);
		return;
	}
#if CONFIG_REF_TRIM
	if (s->id == UI_SET_REF_TRIM && s->type == ST_NUMBER) {
		ui_push(SCR_REF_TRIM, 0);
		return;
	}
#endif
	int32_t v = ui_svc_setting_get(s->id);
	switch (s->type) {
	case ST_TOGGLE:
		set_store(s, !v);
		break;
	case ST_LIST: {
		uint8_t k = 0;
		while (k < s->n && s->values[k] != v)
			k++;
		set_store(s, s->values[(k + 1u) % s->n]);
		break;
	}
	case ST_NUMBER:
		if (set_editing) {
			set_editing = false;
			set_store(s, set_value);
		} else {
			set_editing = true;
			set_typed = false;
			set_value = v;
		}
		break;
	case ST_ACTION:
		switch (s->id) {
		case SA_DIAG:
			if (CONFIG_DIAG)
				ui_push(SCR_DIAG, 0);
			else
				ui_toast(TR("Diagnostics: not in this build"));
			break;
		case SA_FIRMWARE: ui_push(SCR_ABOUT, 0); break;
		case SA_CODEPLUG: ui_push(SCR_INFO, INFO_CODEPLUG); break;
		case SA_MY_RADIO: ui_push(SCR_DMR_ID, 0); break;
		case SA_RESET: ui_push(SCR_CONFIRM, 0); break;
		case SA_LANGUAGE: ui_push(SCR_LANGUAGE, 0); break;
#if CONFIG_CRASH_SCREEN
		case SA_CRASH: ui_push(SCR_CRASH, 0); break;
#endif
		default: break;
		}
		break;
	default:
		break;
	}
	if (hold && !set_editing && s->type != ST_ACTION)
		ui_toast(TR("Settings are saved as you change them"));
}

static bool group_key(const input_event_t *ev)
{
	if (!set_editing)
		return false;
	const set_item_t *s = set_item_at(ui_frame()->focus);
	if (s == NULL) {
		set_editing = false;
		return false;
	}
	int32_t lo, hi;
	set_range(s, &lo, &hi);
	int dir = ev->action == ACT_NAV_UP ? 1 : ev->action == ACT_NAV_DOWN ? -1 :
		ev->action == ACT_ENC_STEP ? (ev->value > 0 ? 1 : -1) : 0;
	if (dir != 0 && ev->gesture != GESTURE_UP) {
		int32_t v = set_value + dir * s->step;
		set_value = v < lo ? lo : v > hi ? hi : v;
		ui_dirty();
		return true;
	}
	// Digits type the value (a key, a level): each one appended, starting over when the number would be too big
	if (ev->action >= ACT_DIGIT_0 && ev->action <= ACT_DIGIT_9 &&
			(ev->gesture == GESTURE_PRESS || ev->gesture == GESTURE_LONG)) {
		int32_t d = ev->action - ACT_DIGIT_0;
		int32_t v = set_typed ? set_value * 10 + d : d;
		set_value = v > hi ? d : v;
		set_typed = true;
		if (set_value < lo && set_value * 10 > hi)
			set_value = lo;	// can't grow into the range any more
		ui_dirty();
		return true;
	}
	if (ev->action == ACT_BACK && ev->gesture == GESTURE_PRESS) {
		set_editing = false;	// BACK leaves the editor without changing the setting
		ui_dirty();
		return true;
	}
	if (ev->action == ACT_SELECT && (ev->gesture == GESTURE_PRESS || ev->gesture == GESTURE_LONG)) {
		set_editing = false;
		if (set_value < lo || set_value > hi || (s->step > 1 && (set_value - lo) % s->step != 0)) {
			// A typed value outside the range or between the steps: say what fits instead of guessing
			static char msg[40];
			sbuf_t b;
			sb_init(&b, msg, sizeof msg);
			sb_str(&b, TR("Not changed: "));
			sb_i32(&b, lo);
			sb_str(&b, TR(" to "));
			sb_i32(&b, hi);
			if (s->step > 1) {
				sb_str(&b, TR(", steps of "));
				sb_i32(&b, s->step);
			}
			ui_toast(msg);
		} else {
			set_store(s, set_value);
		}
		ui_dirty();
		return true;
	}
	return false;
}

static void group_enter(void) { set_editing = false; }

static const char *group_title(void)
{
	uint8_t g = (uint8_t)ui_frame()->arg;
	return g < GROUPS ? tr(groups[g].title) : "";
}

static const ui_scr_def_t group_screen = {
	.name = "group", .flags = SF_LIST, .hint = TRN("OK change  Back"),
	.count = group_count, .item = group_item, .select = group_select, .key = group_key, .enter = group_enter,
	.title = group_title,
};

// ======================= Confirmation (destructive only, defaults to No; P7) =======================

static uint16_t confirm_count(void) { return 2; }

static void confirm_item(uint16_t i, ui_item_t *it)
{
	str_copy(it->label, sizeof it->label, i == 0 ? TR("No, keep settings") : TR("Yes, reset"));
	if (i == 1)
		it->flags |= UI_ITEM_WARN;
}

static void confirm_select(uint16_t i, bool hold)
{
	(void)hold;
	if (i == 0) {
		ui_pop();
	} else if (ui_svc_factory_reset()) {
		ui_go_home();
		ui_toast(TR("Factory settings restored"));
	} else {
		ui_toast(TR("Reset: not in this build"));
	}
}

static const char *confirm_body(void)
{
	return TR("Reset all settings to factory defaults? Channels stay. This can't be undone.");
}

static const ui_scr_def_t confirm_screen = {
	.name = "confirm", .flags = SF_LIST, .prefix = TRN("Factory Reset"), .hint = TRN("OK choose  Back cancel"),
	.count = confirm_count, .item = confirm_item, .select = confirm_select, .body = confirm_body,
};
#else
uint8_t ui_set_group_count(void) { return 0; }
const char *ui_set_group_name(uint8_t g) { (void)g; return ""; }
uint8_t ui_set_item_count(uint8_t g) { (void)g; return 0; }
const char *ui_set_item_name(uint8_t g, uint8_t i, const char **synonyms) { (void)g; (void)i; *synonyms = NULL; return ""; }
void ui_open_setting(uint8_t g, uint8_t i) { (void)g; (void)i; }

static uint16_t settings_count(void) { return 1; }
static void settings_item(uint16_t i, ui_item_t *it)
{
	(void)i;
	str_copy(it->label, sizeof it->label, TR("Set them with qdmr"));
	it->flags |= UI_ITEM_INFO;
}

static const ui_scr_def_t settings_screen = {
	.name = "settings", .flags = SF_LIST, .prefix = TRN("Settings"), .hint = TRN("Back"),
	.count = settings_count, .item = settings_item,
};
#define group_screen	settings_screen
#define confirm_screen	settings_screen
#endif

// ======================= Talk target picker (SK2, quick 8) =======================

#define PICK_MAX	12
typedef struct {
	ui_target_t t;
	const char *tag;
} pick_t;
static pick_t picks[PICK_MAX];
static uint8_t n_picks;


static void picker_enter(void)
{
	n_picks = 0;
	// Focus the reply target when there is one (task-flows.md 3: "TG 91 Reply" second); the tags are kept in
	// English for this
	for (uint8_t i = 0; i < n_picks; i++)
		if (picks[i].tag[0] == 'R' && picks[i].tag[1] == 'e' && picks[i].tag[2] == 'p')
			ui_frame()->focus = i;
}

// After the targets: Type a number..., then Select contact... (every contact, as Contacts lists them)
#define PICK_CONTACTS	(CONFIG_DMR ? 1u : 0u)

static uint16_t picker_count(void) { return (uint16_t)(n_picks + 1u + PICK_CONTACTS); }

static void picker_item(uint16_t i, ui_item_t *it)
{
	if (i >= n_picks) {
		str_copy(it->label, sizeof it->label, i == n_picks ? TR("Type a number...") : TR("Select contact..."));
		return;
	}
	ui_sb_target(it->label, sizeof it->label, &picks[i].t);
	sbuf_t b;	// the tag, left out where the target needs the room
	sb_init(&b, it->value, sizeof it->value);
	sb_str(&b, TEXT_ALT_OPEN);
	sb_str(&b, tr(picks[i].tag));
	sb_str(&b, TEXT_ALT_NEXT TEXT_ALT_CLOSE);
}

static bool picker_target(uint16_t i, ui_target_t *t)
{
	if (i >= n_picks)
		return false;
	*t = picks[i].t;
	return true;
}

static void picker_select(uint16_t i, bool hold)
{
	if (i > n_picks) {
		ui_push(SCR_CONTACTS, 0);	// PTT, OK, hold OK and # work there as here
		return;
	}
	if (i == n_picks) {
		ui_push(SCR_FIND, 0);	// digits then OK / PTT: talk to the number typed
		ui_find_set("");
		return;
	}
	ui_talk(&picks[i].t, hold);
}

static void picker_context(uint16_t i)
{
	if (i < n_picks)
		ui_open_actions(&picks[i].t);
}

static const char *picker_title(void)
{
	return ui_view()->ch.name;
}

static const ui_scr_def_t picker_screen = {
	.name = "picker", .flags = SF_LIST | SF_FIELD, .prefix = TRN("Talk to"),
	.hint = TRN(TEXT_ALT_OPEN "PTT once  OK use  Hold OK set" TEXT_ALT_NEXT "OK use  Hold OK set" TEXT_ALT_CLOSE),
	.count = picker_count, .item = picker_item, .select = picker_select, .context = picker_context,
	.target = picker_target, .enter = picker_enter, .title = picker_title,
};

// ======================= Scan: what to do with the channel that stopped it =======================

static uint16_t scan_count(void) { return 3; }

static void scan_item(uint16_t i, ui_item_t *it)
{
	static const char *const names[] = { TRN("Skip channel for now"), TRN("Stay here"), TRN("Always skip") };
	str_copy(it->label, sizeof it->label, tr(names[i]));
	item_num(it, (uint16_t)(i + 1u));
	if (i == 2)
		it->flags |= UI_ITEM_DIM;
}

static void scan_select(uint16_t i, bool hold)
{
	(void)hold;
	if (i == 0) {
		ui_go_home();
		ui_toast(ui_svc_scan_skip() ? TR("Skipped until the scan stops") : TR("Nothing to skip yet"));
	} else if (i == 1) {
		ui_svc_scan(false);
		ui_st.scanning = false;
		ui_go_home();
		ui_toast(TR("Scan stopped here"));
	} else {
		ui_toast(TR("Edit scan lists in qdmr"));
	}
}

static const ui_scr_def_t scan_screen = {
	.name = "scan", .flags = SF_LIST | SF_NUMBERED | SF_FIELD, .prefix = TRN("Scan"), .hint = TRN("1-3 pick  Back"),
	.count = scan_count, .item = scan_item, .select = scan_select,
};

// ======================= Screens with their own drawing =======================

static const ui_scr_def_t home_screen = {
	.name = "home", .flags = SF_FIELD, .key = ui_home_key, .draw = ui_home_draw,
};
static const ui_scr_def_t quick_screen = {
	.name = "quick", .flags = SF_FIELD, .key = ui_quick_key, .draw = ui_quick_draw, .enter = ui_quick_enter,
};
static const ui_scr_def_t diag_screen = {
	.name = "diag", .flags = 0, .key = ui_diag_key, .draw = ui_diag_draw,
};
static const ui_scr_def_t volume_screen = {
	.name = "volume", .flags = SF_FIELD, .key = ui_volume_key, .draw = ui_volume_draw,
};
static const ui_scr_def_t emergency_screen = {
	.name = "emergency", .flags = SF_FIELD, .key = ui_emergency_key, .draw = ui_emergency_draw,
};

// ======================= Settings > Display > Language =======================
// Every language a catalog exists for, each in its own name; one this image was built without is dimmed and says so
// (the flasher builds it in, docs/user)

static uint16_t lang_list_count(void) { return lang_count(); }

static void lang_list_item(uint16_t i, ui_item_t *it)
{
	str_copy(it->label, sizeof it->label, lang_name((uint8_t)i));
	if (i == lang_current())
		str_copy(it->value, sizeof it->value, TR("In use"));
	if (!lang_built((uint8_t)i))
		it->flags |= UI_ITEM_DIM;
}

static void lang_list_select(uint16_t i, bool hold)
{
	(void)hold;
	if (!lang_built((uint8_t)i)) {
		ui_toast(TR("Not in this build"));	// a fixed text: no RAM for a buffer naming the language
		return;
	}
	if (!ui_svc_setting_set(UI_SET_LANGUAGE, lang_value((uint8_t)i))) {
		ui_toast(TR("Couldn't save: try again"));
		return;
	}
	ui_apply_setting(UI_SET_LANGUAGE, lang_value((uint8_t)i));
	ui_pop();
}

static void lang_list_enter(void)
{
	ui_frame()->focus = lang_current();
}

static const ui_scr_def_t lang_screen = {
	.name = "language", .flags = SF_LIST, .prefix = TRN("Language"), .hint = TRN("OK pick  Back"),
	.count = lang_list_count, .item = lang_list_item, .select = lang_list_select, .enter = lang_list_enter,
};

// ======================= Another firmware's codeplug: the first edit asks (defaults to Cancel; P7) ===============
// ui_svc_codeplug_answer: the rows are UI_CP_CANCEL, UI_CP_NEW, UI_CP_USE. BACK is Cancel (ui_pop drops the write)

static uint16_t cpf_count(void) { return 3; }

static void cpf_item(uint16_t i, ui_item_t *it)
{
	str_copy(it->label, sizeof it->label, i == UI_CP_CANCEL ? TR("Cancel") : i == UI_CP_NEW ? TR("Start new") :
		 TR("Use anyway"));
	if (i == UI_CP_NEW)
		it->flags |= UI_ITEM_WARN;
}

static void cpf_select(uint16_t i, bool hold)
{
	(void)hold;
	if (i != UI_CP_CANCEL && !ui_svc_codeplug_answer((uint8_t)i)) {
		ui_toast(TR("Busy saving: try again"));
		return;
	}
	ui_pop();	// Cancel: the pop drops the write and says so
	if (i != UI_CP_CANCEL)
		ui_toast(i == UI_CP_NEW ? TR("New codeplug started") : TR("Old codeplug kept: edit again"));
}

static const char *cpf_body(void)
{
	return TR("Start new replaces the radio's channel settings. Back up first.");
}

static const ui_scr_def_t cpf_screen = {
	.name = "codeplug-foreign", .flags = SF_LIST, .prefix = TRN("Old codeplug"), .hint = TRN("OK choose  Back cancel"),
	.count = cpf_count, .item = cpf_item, .select = cpf_select, .body = cpf_body,
};

const ui_scr_def_t *const ui_scr[SCR_COUNT] = {
	[SCR_HOME] = &home_screen, [SCR_QUICK] = &quick_screen, [SCR_HUB] = &hub_screen,
	[SCR_FIND] = &ui_find_screen, [SCR_ACTIONS] = &actions_screen, [SCR_CONTACTS] = &contacts_screen,
	[SCR_MESSAGES] = &messages_screen, [SCR_MESSAGE] = &message_screen, [SCR_COMPOSE] = &compose_screen,
	[SCR_RECENT] = &recent_screen, [SCR_ZONES] = &channels_screen, [SCR_ZONE] = &zone_screen,
	[SCR_TOOLS] = &tools_screen, [SCR_SETTINGS] = &settings_screen, [SCR_GROUP] = &group_screen,
	[SCR_PICKER] = &picker_screen, [SCR_INFO] = &info_screen, [SCR_CONFIRM] = &confirm_screen,
	[SCR_DIAG] = &diag_screen, [SCR_EMERGENCY] = &emergency_screen, [SCR_SCAN] = &scan_screen,
	[SCR_ROAM] = &roam_screen, [SCR_APRS] = &ui_aprs_screen, [SCR_APRS_HEARD] = &ui_aprs_heard_screen,
	[SCR_APRS_POS] = &ui_aprs_pos_screen, [SCR_ABOUT] = &about_screen,
	[SCR_VOLUME] = &volume_screen, [SCR_LANGUAGE] = &lang_screen,
	[SCR_DMR_ID] = &dmr_id_screen,
	[SCR_CP_FOREIGN] = &cpf_screen,
#if CONFIG_REF_TRIM
	[SCR_REF_TRIM] = &trim_screen,
#endif
#if CONFIG_CHANNEL_LIST
	[SCR_CHANNELS] = &chlist_screen, [SCR_CHANNEL] = &chinfo_screen,
#if CONFIG_CHANNEL_EDIT
	[SCR_TEXT] = &text_screen, [SCR_DISCARD] = &discard_screen, [SCR_CHDELETE] = &chdelete_screen,
#if CONFIG_ZONE_EDIT
	[SCR_ZCHANNEL] = &zchan_screen, [SCR_ZPICK] = &zpick_screen, [SCR_ZONEEDIT] = &zopt_screen,
	[SCR_ZONEDEL] = &zdel_screen, [SCR_CHZONES] = &chz_screen,
#endif
#endif
#endif
#if CONFIG_CRASH_SCREEN
	[SCR_CRASH] = &crash_screen,
#endif
#if CONFIG_DUAL_WATCH
	[SCR_DUAL] = &ui_dual_screen, [SCR_DUAL_PICK] = &ui_dual_pick_screen,
	[SCR_DUAL_ZONE] = &ui_dual_zone_screen,
#endif
};
