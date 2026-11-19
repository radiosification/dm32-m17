/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "m17.h"
enum { M17_PCM_SAMPLES=320, M17_FRAME_SYMBOLS=192 };
typedef void (*m17_payload_sink)(void *, const uint8_t payload[16], uint16_t fn);
typedef struct {
 lsf_t lsf, partial;
 viterbi_ctx_t fec;
 m17_payload_sink sink;
 void *ctx;
 uint8_t lich_mask;
 bool have_lsf, ended, have_fn;
 uint16_t last_fn;
 uint32_t frames, bad, lost, duplicates, lsf_count;
} m17_receiver;
void m17_receiver_init(m17_receiver *,m17_payload_sink,void *);
int m17_receive_frame(m17_receiver *,const int16_t symbols[192]);
void m17_pack_phy(const int8_t symbols[192],uint8_t out[48]);
void m17_unpack_phy(const uint8_t in[48],int16_t symbols[192]);
/* Native modem banks contain 36 bytes / 144 dibits, while M17 frames contain
 * 48 bytes / 192 dibits. Keep framing across arbitrary bank boundaries. */
typedef struct {
 m17_receiver *rx;
 int16_t frame[192], history[8];
 unsigned count, history_count;
 bool collecting;
} m17_symbol_stream;
void m17_symbol_stream_init(m17_symbol_stream *,m17_receiver *);
void m17_receive_phy_bytes(m17_symbol_stream *,const uint8_t *,unsigned);
typedef struct { int8_t symbols[9];unsigned pos, tick; } m17_modulator;
void m17_mod_init(m17_modulator *);
void m17_modulate(m17_modulator *,const int8_t symbols[192],int16_t pcm[320]);
typedef struct {
 int16_t samples[128], filtered[128], frame[192];
 uint32_t ticks, candidate_end, best_tick, next_symbol;
 unsigned pos, frame_n;
 uint32_t best_error;
 bool candidate, collecting;
 m17_receiver *rx;
} m17_demodulator;
void m17_demod_init(m17_demodulator *,m17_receiver *);
void m17_demodulate(m17_demodulator *,const int16_t *,unsigned);
