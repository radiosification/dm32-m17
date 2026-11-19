// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// DM-32UV diagnostics page lines: raw readings for bench checks (cached values only, no bus traffic)
#include <stddef.h>
#include "dm32.h"
#include "config.h"
#if CONFIG_M17
#include "dv/radio_m17.h"
#include "audio_pcm.h"
#include "fm_rx_audio.h"
#if CONFIG_WATCHDOG
#include "app/watchdog.h"
#endif
#endif
#include "platform.h"

#if CONFIG_DIAG

typedef struct {
	char *p, *end;
} out_t;

static void put(out_t *o, const char *s)
{
	while (*s && o->p < o->end) {
		*o->p++ = *s++;
	}
}

static void put_u(out_t *o, uint32_t v)
{
	char t[11];
	int n = 0;
	do {
		t[n++] = (char)('0' + v % 10);
		v /= 10;
	} while (v && n < 10);
	while (n && o->p < o->end) {
		*o->p++ = t[--n];
	}
}

static void put_x(out_t *o, uint32_t v, int digits)
{
	for (int i = digits - 1; i >= 0 && o->p < o->end; i--) {
		*o->p++ = "0123456789ABCDEF"[(v >> (4 * i)) & 0xF];
	}
}

bool dm32_diag_line(uint8_t i, char *buf, uint8_t len)
{
	if (len == 0) {
		return false;
	}
	out_t o = { buf, buf + len - 1 };
	dm32_radio_diag_t d;
	switch (i) {
	case 0:
		put(&o, "bat ");
		put_u(&o, hrc_adc_last(2));
		put(&o, " ");
		put_u(&o, board.power->battery_mv());
		put(&o, "mV");
		break;
	case 1:
		dm32_radio_diag(&d);
		put(&o, "rssi ");
		put_u(&o, d.rssi_raw);
		put(&o, " nz ");
		put_u(&o, d.noise_raw);
		put(&o, " fm ");
		put_u(&o, d.fm_noise);
		break;
	case 2:
		dm32_radio_diag(&d);
		put(&o, "fd wr ");
		put_u(&o, d.fd_writes);
		put(&o, " uo ");
		put_u(&o, hrc_host_uart_overruns());
		put(&o, " ue ");
		put_u(&o, hrc_host_uart_line_errors());
		break;
	case 3: {	// host port traffic: which side stopped when the link goes quiet
		hrc_uart_traffic_t t;
		hrc_host_uart_traffic(&t);
		put(&o, "ur ");
		put_u(&o, t.rx_bytes);
		put(&o, " ut ");
		put_u(&o, t.tx_bytes);
		put(&o, " ts ");
		put_u(&o, t.tx_stalls);
		break;
	}
	case 4: {
		hrc_uart_traffic_t t;
		uint32_t ms;
		hrc_host_uart_traffic(&t);
		put(&o, "hb ");
		put_u(&o, t.waits);
		put(&o, " ie ");
		put_x(&o, t.ier, 2);
		put(&o, " fl ");
		put(&o, hrc_flash_spi0_holder(&ms));
		put(&o, " ");
		put_u(&o, ms);
		break;
	}
	case 5:
		put(&o, "flash ");
		put_x(&o, hrc_flash_spi0_jedec(), 6);
		put(&o, " cal ");
		put(&o, bf_cal_source());
		break;
	case 6:
		put(&o, "txc ");
		put_u(&o, bf_cal_tx_center());
		put(&o, " mod ");
		put_x(&o, bf_cal_modcal(145000000u), 4);
		put(&o, " ");
		put_x(&o, bf_cal_modcal(435000000u), 4);
		break;
	case 7:	// PA drive, power level 1 (low) and 8 (high) at 145 / 435 MHz
		put(&o, "pa ");
		put_u(&o, bf_cal_power_level(145000000u, 1));
		put(&o, "/");
		put_u(&o, bf_cal_power_level(145000000u, 8));
		put(&o, " ");
		put_u(&o, bf_cal_power_level(435000000u, 1));
		put(&o, "/");
		put_u(&o, bf_cal_power_level(435000000u, 8));
		break;
	case 8: {	// GPS (platform/hrc7000/gps_hrc.c): module heard, valid / bad sentences, bytes received
#if CONFIG_GPS
		uint32_t good, bad, n;
		hrc_gps_counts(&good, &bad, &n);
		put(&o, hrc_gps_present() ? "gps yes " : "gps no ");
		put_u(&o, good);
		put(&o, "/");
		put_u(&o, bad);
		put(&o, " b");
		put_u(&o, n);
		put(&o, hrc_gps_running() ? " on" : " off");
#else
		put(&o, "gps not built");
#endif
		break;
	}
	case 9:	// key scans with the PTT pin low taken for host serial data (hardware-dm32.md 6.2)
		put(&o, "ptt ign ");
		put_u(&o, dm32_input_ptt_ignored());
		break;
#if CONFIG_M17
    case 10: case 11: case 12: {
        uint32_t v[6];radio_m17_counters(v);
        if(i==10){put(&o,"M17 RX max us ");put_u(&o,v[3]);}
        if(i==11){put(&o,"M17 TX max us ");put_u(&o,v[4]);}
        if(i==12){put(&o,"M17 ov/un ");put_u(&o,v[1]);put(&o,"/");put_u(&o,v[2]);}
        break;
    }
    case 13: {
        uint32_t b,ov,un;hrc_audio_pcm_stats(&b,&ov,&un);
        put(&o,"PCM blocks/ov ");put_u(&o,b);put(&o,"/");put_u(&o,ov);
        break;
    }
#if CONFIG_WATCHDOG
    case 14:
        put(&o,"Idle gap ms ");put_u(&o,watchdog_state()->max_gap_ms);
        break;
#endif
    case 15: {
        uint32_t good,bad,lost;
        radio_m17_rx_counters(&good,&bad,&lost);
        put(&o,"RF ok/bad/lost ");put_u(&o,good);put(&o,"/");put_u(&o,bad);put(&o,"/");put_u(&o,lost);
        break;
    }
    case 16: {
        uint32_t banks,dropped;
        hrc_fm_rx_stats(&banks,&dropped);
        put(&o,"RX banks/drop ");put_u(&o,banks);put(&o,"/");put_u(&o,dropped);
        break;
    }
    case 17: {
        uint32_t late,max_us;
        hrc_fm_rx_timing(&late,&max_us);
        put(&o,"RX bank late/us ");put_u(&o,late);put(&o,"/");put_u(&o,max_us);
        break;
    }
    case 18: {
        uint32_t playback[16];
        board.audio->playback_stats(playback);
        put(&o,"FIFO max gap us ");put_u(&o,playback[6]);
        break;
    }
#endif
	default:
		return false;
	}
	*o.p = '\0';
	return true;
}

void dm32_diag_reset(void)
{
#if CONFIG_M17
	hrc_audio_pcm_reset_stats();
#endif
#if CONFIG_M17 && CONFIG_WATCHDOG
    watchdog_clear_gap();
#endif
}

#endif
