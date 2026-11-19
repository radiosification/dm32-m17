/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "radio_dv.h"
bool radio_m17_select(const radio_dv_cfg_t *);
bool radio_m17_ptt(bool);
void radio_m17_service(uint32_t);
void radio_m17_status(radio_dv_status_t *,uint32_t);

bool radio_m17_identity_valid(void);


void radio_m17_counters(uint32_t v[6]);
void radio_m17_rx_counters(uint32_t *good, uint32_t *bad, uint32_t *lost);
