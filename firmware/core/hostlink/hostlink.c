// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The host link task (see hostlink.h)
#include "config.h"
#include "hostlink/hostlink.h"
#include "board.h"
#include "hal_time.h"
#if CONFIG_DEBUGLINK
#include "debuglink/debuglink.h"
#elif CONFIG_STUB_ENTRY
#include "stubentry/stub_entry.h"
#endif
#if CONFIG_CPS_FAST
#include "cps/cps_fast.h"
#endif
#if CONFIG_APRS
#include "aprs/aprs_host.h"
#endif
#include "debuglink/dlog.h"
#include "settings/settings.h"
#include "FreeRTOS.h"
#include "task.h"

#define HOSTLINK_TASK_PRIORITY	3	// with input and USB; above the UI (core/app/app.c task table)
#if CONFIG_DEBUGLINK
// words: MMDVM frames on the stack; debug link replies format diagnostics lines. The deepest chain (the web view's
// debug link section > the DMR data view, with the air queue inlined into the task) reads 2568 bytes with the macOS
// toolchain; the CI's reads up to 16 deeper, and stack-check.md's overhead and margin come on top, 4 to spare
#define HOSTLINK_TASK_STACK	731
#else
#define HOSTLINK_TASK_STACK	512	// words: MMDVM frames are built on the stack
#endif
#define POLL_MS			5	// hotspot housekeeping while nothing arrives
#define READ_CHUNK		64
#define CPS_GAP_MS		3	// a new CPS request starts after this much quiet (open-firmware-cps.md 1.2)
#define PPP_FLAG		0x7E	// starts every PPP frame (RFC 1662); no other protocol here starts a request with it

#ifndef RFOUNDRY_GIT
#define RFOUNDRY_GIT "unknown"
#endif

static volatile hostlink_mode_t mode = HOSTLINK_IDLE;
static volatile hostlink_leave_t last_leave = HOSTLINK_STAY;
static volatile bool hotspot_allowed = true;
static volatile bool exit_requested;
static volatile int8_t usb_switch = -1;		// a personality switch asked for: 1 network, 0 serial, -1 none
static hostlink_cps_fn cps_handler;
static bool (*cps_mid_request)(void);
static const hostlink_side_t *side;
static const hostlink_tnc_t *tnc;
static uint32_t last_byte_ms;
static uint32_t last_main_ms;		// the last byte for the mode's own protocol (side frames don't count)
static bool side_ended;			// a side frame just ended: the next byte is at a request boundary

hostlink_mode_t hostlink_mode(void)
{
	return mode;
}

hostlink_leave_t hostlink_last_leave(void)
{
	return last_leave;
}

void hostlink_set_hotspot_allowed(bool allowed)
{
	hotspot_allowed = allowed;
	if (!allowed) {
		exit_requested = true;
	}
}

void hostlink_hotspot_exit(void)
{
	exit_requested = true;
}

bool hostlink_request_usb_network(bool network)
{
	if ((board.netif == NULL) || (board.netif->select == NULL)) {
		return false;
	}
	usb_switch = network ? 1 : 0;
	return true;
}

bool hostlink_network_mode(void)
{
	return (board.netif != NULL) && board.netif->selected();
}


void hostlink_set_cps_handler(hostlink_cps_fn fn, bool (*mid_request)(void))
{
	cps_handler = fn;
	cps_mid_request = mid_request;
}

void hostlink_set_side_protocol(const hostlink_side_t *p)
{
	side = p;
}

void hostlink_set_tnc(const hostlink_tnc_t *t)
{
	tnc = t;
}

size_t hostlink_write(const uint8_t *buf, size_t len)
{
	if (board.host_port == NULL) {
		return 0;
	}
	return board.host_port->write(buf, len);
}


// Bytes of the side protocol at the start of buf, if they are its own (see hostlink.h for where a frame may start)
static size_t side_take(const uint8_t *buf, size_t len, uint32_t now, bool new_request)
{
	if (side == NULL) {
		return 0;
	}
	if (!side->in_frame(now)) {
		if (buf[0] != side->start) {
			return 0;
		}
		switch (mode) {
		case HOSTLINK_HOTSPOT:
			break;
		case HOSTLINK_CPS:
			if (!new_request) {
				return 0;
			}
			break;
		case HOSTLINK_TNC:
			if (tnc->in_frame()) {
				return 0;
			}
			break;
		case HOSTLINK_IDLE:
			break;
		}
	}
	return side->feed(buf, len, now);
}

// The other protocols: returns the bytes taken (all of them, except in hotspot mode before a side frame)
static size_t dispatch_main(const uint8_t *buf, size_t len, uint32_t now, bool new_request)
{
	if ((tnc != NULL) && (mode == HOSTLINK_IDLE) && new_request && tnc->claims(buf[0])) {
		mode = HOSTLINK_TNC;
		dlog("hostlink: TNC (KISS)");
	}
	if (mode == HOSTLINK_TNC) {
		tnc->feed(buf, len, now);
		return len;
	}
	if (cps_handler != NULL) {
		mode = HOSTLINK_CPS;
		cps_handler(buf, len, now);
	}
	return len;
}

static void dispatch(const uint8_t *buf, size_t len, uint32_t now)
{
	// A side frame's end is a request boundary too, also when the next request comes in a later chunk without the
	// gap (a debug link client sends its next request as soon as the reply is in)
	// A CPS request the engine holds part of continues after a gap: a UART delivers it in pieces, and a debug link or
	// MMDVM start byte in its rest is data (a cable on a UART host port)
	bool cps_mid = (mode == HOSTLINK_CPS) && (cps_mid_request != NULL) && cps_mid_request();
	bool new_request = side_ended || (((now - last_byte_ms) >= CPS_GAP_MS) && !cps_mid);

	last_byte_ms = now;
	while (len > 0) {
		size_t n = side_take(buf, len, now, new_request);

		if (n > 0) {
			side_ended = !side->in_frame(now);
			new_request = side_ended;	// what follows a side frame starts a new request
		} else {
			side_ended = false;
			last_main_ms = now;
			n = dispatch_main(buf, len, now, new_request);
			new_request = false;
			if (n == 0) {
				break;
			}
		}
		buf += n;
		len -= n;
	}
}



// A personality switch asked for (the debug link's reply is out by now): hotspot mode ends first
static void usb_switch_now(void)
{
	int8_t s = usb_switch;

	if (s < 0) {
		return;
	}
	usb_switch = -1;
	mode = HOSTLINK_IDLE;
	dlog("hostlink: USB %s", s ? "network" : "serial");
	hal_delay_ms(20);	// the reply's last USB packet
	board.netif->select(s != 0);
}


void hostlink_settings_changed(void)
{
}

static void hostlink_task(void *arg)
{
	(void)arg;
	const hal_stream_t *port = board.host_port;
	uint8_t buf[READ_CHUNK];

	if ((port == NULL) || !port->start()) {
		vTaskDelete(NULL);
		return;
	}

	for (;;) {
		bool more = port->wait_rx(POLL_MS);
		uint32_t now = hal_millis();
		bool exit_now = exit_requested;

		exit_requested = false;
		(void)exit_now;

		while (more) {
			size_t n = port->read(buf, sizeof(buf));

			if (n == 0) {
				break;
			}
			dispatch(buf, n, now);
			now = hal_millis();
		}
#if CONFIG_CPS_FAST
		cps_fast_tick(now);		// the fast codeplug protocol: session time-out, a commit's next sector
#endif
		usb_switch_now();

		switch (mode) {
		case HOSTLINK_HOTSPOT:
			mode = HOSTLINK_IDLE;
			break;
		case HOSTLINK_CPS:
			if (cps_handler != NULL) {
				cps_handler(NULL, 0, now);	// tick: gap-terminated requests, the session time-out
			}
			if ((now - last_main_ms) > HOSTLINK_CPS_IDLE_MS) {	// debug link traffic doesn't hold it
				mode = HOSTLINK_IDLE;
			}
			break;
		case HOSTLINK_TNC:
			if ((now - last_main_ms) > HOSTLINK_TNC_IDLE_MS) {
				mode = HOSTLINK_IDLE;
			}
			break;
		case HOSTLINK_IDLE:
			break;
		}
		bool tnc_out = true;
		if ((tnc != NULL) && tnc_out && ((mode == HOSTLINK_IDLE) || (mode == HOSTLINK_TNC))) {
			size_t n;

			while ((n = tnc->output(buf, sizeof(buf))) > 0) {	// received packets, iGate lines
				hostlink_write(buf, n);
			}
		}
	}
}

void hostlink_start(void)
{
#if CONFIG_DEBUGLINK
	debuglink_start();	// registers itself as the side protocol; hands STUB frames to the stub entry
#elif CONFIG_STUB_ENTRY
	stub_entry_start();	// the stub entry's own listener on the same framing (docs/spec/stub-entry.md)
#endif
#if CONFIG_APRS
	aprs_host_start();	// registers the KISS TNC
#endif
	xTaskCreate(hostlink_task, "host", HOSTLINK_TASK_STACK, NULL, HOSTLINK_TASK_PRIORITY, NULL);
}
