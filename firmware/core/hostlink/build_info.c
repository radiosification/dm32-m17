// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// What this build is, for VERSION in either listener (see build_info.h)
#include "config.h"
#include "hostlink/build_info.h"

#ifndef RFOUNDRY_PRESET
#define RFOUNDRY_PRESET "unknown"
#endif
#ifndef RFOUNDRY_FEATURES_OFF
#define RFOUNDRY_FEATURES_OFF ""
#endif

const char build_preset[] = RFOUNDRY_PRESET;
const char build_off[] = RFOUNDRY_FEATURES_OFF;

const char build_features[] =
#if CONFIG_FM
	"FM,"
#endif
#if CONFIG_SUBTONE
	"SUBTONE,"
#endif
#if CONFIG_APRS
	"APRS,"
#endif
#if CONFIG_APRS_RX
	"APRS_RX,"
#endif
#if CONFIG_VFO
	"VFO,"
#endif
#if CONFIG_VFO_FM
	"VFO_FM,"
#endif
#if CONFIG_DIAG
	"DIAG,"
#endif
#if CONFIG_CRASH_LOG
	"CRASH_LOG,"
#endif
#if CONFIG_SCAN
	"SCAN,"
#endif
#if CONFIG_BEEP
	"BEEP,"
#endif
#if CONFIG_FM_TX_TONES
	"FM_TX_TONES,"
#endif
#if CONFIG_DTMF
	"DTMF,"
#endif
#if CONFIG_BATTERY_SAVE
	"BATTERY_SAVE,"
#endif
#if CONFIG_CPS_FAST
	"CPS_FAST,"
#endif
#if CONFIG_DEBUGLINK
	"DEBUGLINK,"
#endif
	"HOSTLINK";	// both listeners run on the host link (USB CDC or a UART)
