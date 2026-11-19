/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Dispatch digital voice to the M17 modem. */
#include "radio_dv.h"
#include "config.h"
#include "hal_radio.h"
#include "radio/radio.h"
#include "radio_dv_reasons.h"
#include <string.h>
#if CONFIG_M17
#include "radio_m17.h"
#endif


static radio_dv_cfg_t current;
static bool selected;

bool radio_dv_select(const radio_dv_cfg_t *cfg)
{
    if (cfg && selected && !memcmp(cfg, &current, sizeof current)) return true;
    if (selected) {
        /* Another channel, or new call settings: the receiver starts over. */
#if CONFIG_M17
        if (current.mode == RADIO_MODE_M17) radio_m17_select(NULL);
#endif
        selected = false;
    }
    if (!cfg) return true;
    bool ok = false;
#if CONFIG_M17
    if (cfg->mode == RADIO_MODE_M17) ok = radio_m17_select(cfg);
#endif
    if (!ok) return false;
    current = *cfg;
    selected = true;
    return true;
}

bool radio_dv_ptt(bool down)
{
    if (!selected) return false;
#if CONFIG_M17
    if (current.mode == RADIO_MODE_M17) return radio_m17_ptt(down);
#endif
    (void)down;
    return false;
}

void radio_dv_status(radio_dv_status_t *o, uint32_t now_ms)
{
    memset(o, 0, sizeof *o);
    if (!selected) return;
#if CONFIG_M17
    if (current.mode == RADIO_MODE_M17) radio_m17_status(o, now_ms);
#endif
    (void)now_ms;
}

void radio_dv_service(uint32_t now_ms)
{
#if CONFIG_M17
    if (selected && current.mode == RADIO_MODE_M17) radio_m17_service(now_ms);
#endif
    (void)now_ms;
}

uint8_t radio_dv_tx_deny(uint8_t nt_reason)
{
    switch (nt_reason) {
    case NT_OK:
    case NT_TX_COMPLETE:
    case NT_USER_STOP:
        return TXDENY_NONE;
    case NT_DENIED:
        return TXDENY_REJECTED;    /* the board refused: band, calibration, battery, gate */
    case NT_LEASE:
    case NT_DEADLINE:
    case RADIO_DV_TRUNK_NO_ANSWER:     /* the trunked system did not answer, or queued us too long */
        return TXDENY_TIMEOUT;
    case RADIO_DV_TRUNK_REFUSED:       /* the system denied the call, or we are not registered on it */
        return TXDENY_REJECTED;
    case RADIO_DV_TRUNK_NO_TX:         /* trunked transmit is off in this build or for this channel */
        return TXDENY_MODE;
    default:
        return TXDENY_NO_DMR;      /* the modem failed: shown as "modem" */
    }
}
