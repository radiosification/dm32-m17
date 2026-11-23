/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "codec2_mod.h"
#include "m17_link.h"
typedef void (*m17_speaker_sink)(void *,const int16_t pcm[160]);
/* Half-duplex codec storage: encoder and decoder never coexist on the radio.
 * Initialize in task context, and keep DSP out of interrupt callbacks. */
typedef struct {
 union {codec2_encoder_t encoder;codec2_decoder_t decoder;} codec;
 lsf_t tx_lsf;
 m17_receiver receiver;
 m17_modulator mod;
 m17_demodulator demod;
 m17_symbol_stream symbol_stream;
 m17_speaker_sink speaker;
 bool (*admit)(const lsf_t *); /* Optional filter before decoding. */
 void *ctx;
 uint32_t rx_lsf_count;
 uint16_t seq;
 bool transmitting, rx_voiced;
} m17_voice;
void m17_voice_init(m17_voice *,m17_speaker_sink,void *);
int m17_voice_tx_begin(m17_voice *,const char *source,const char *destination,unsigned can,int8_t lsf[192]);
int m17_voice_tx(m17_voice *,const int16_t speech[320],bool last,int8_t symbols[192]);
void m17_voice_rx_begin(m17_voice *);
int m17_voice_rx_phy(m17_voice *,const uint8_t phy[48]);
void m17_voice_rx_phy_bytes(m17_voice *,const uint8_t *,unsigned);
void m17_voice_rx_pcm(m17_voice *,const int16_t *,unsigned);
