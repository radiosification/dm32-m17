// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The split duplex TX half's continuous BS outbound on the HR_C7000 (CONFIG_HOTSPOT_SPLIT; docs/spec/split-duplex.md
// 5.4, 10.1; modem-hrc7000.md 2.9, 2.10). While it runs it owns the chip in place of the slot engine of
// hrc7000_dmr.c, whose ISRs and watchdog hand over to it (e->bs set).
//
// Framing: the chip's layer 2 BS mode frames Idle bursts with a CACH of valid parity, but its AT, TC and short LC are
// not under our control (2.9, bench 2026-10-20). So the chip runs in the physical layer mode (DS 9.5), where it sends
// the CPU's 288 bits per slot without coding: the core builds each slot (bsOutboundRawSlot: the CACH, then the burst
// with BS sync, slot type and coded payload) and the driver writes it to TX RAM. Own timing (LAYER2_CONTROL txen and
// tx_master_mode) gives a free-running slot clock; a slot is sent when LAYER2_TXRX_CTRL b7 is set at its slot
// interrupt (PHY_RDY or a timeslot vector), which comes about 5 ms before the slot goes on air. The PA is keyed once at the first RF TX interrupt and stays up until unkeyed. Each outbound
// slot plays its own queue in order; an empty one gets an idle burst (counted). Key-up starts with an outbound TS1
// slot.
//
// The variant (dmr_bs_config_t.variant) is a bench experiment's bit field, 0 the default: b0 continuous mode
// (WORK_MODE 0x02) instead of timeslot mode (0x22); b1 LAYER2_TXRX_CTRL b0 autotest set with the send bit, as the
// datasheet's set-up table has it (0x81); b3:2 the TX RAM halves: 0 the slot goes to both (as on the HR_C6000), 1
// alternate, the first slot in SEND_DATA0 (0x00), 2 alternate, the first in SEND_DATA1 (0x24).
#ifndef HRC7000_BS_H
#define HRC7000_BS_H

#include <stdint.h>
#include <stdbool.h>
#include "hrc7000_dmr.h"

#endif
