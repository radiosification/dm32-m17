/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef RADIO_DV_REASONS_H
#define RADIO_DV_REASONS_H

/* Preserve status codes used by the M17 diagnostics. */
enum { NT_OK, NT_USER_STOP, NT_RX_LIMIT, NT_TX_COMPLETE, NT_RX_DURATION,
       NT_LEASE, NT_DEADLINE, NT_STALLED, NT_LATE, NT_EARLY, NT_UNDERRUN,
       NT_IO, NT_RX_OVERFLOW, NT_ABNORMAL, NT_DENIED, NT_INTERRUPTED,
       NT_RESTORE, NT_UNEXPECTED_RF };

#endif
