/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdbool.h>
#include <stdint.h>
void m17_fixed_decoder_init(void *);
/* Returns true if either decoded half contains voiced speech. */
bool m17_fixed_decode(void *,int16_t *,const uint8_t *);
void m17_fixed_encoder_init(void *);
void m17_fixed_encode(void *,uint8_t *,const int16_t *);
/* Integer FFT bridges. Values retain their input binary point. */
void m17_fixed_ifft(const int32_t *,const int32_t *,int32_t *);
int m17_fixed_fft(const int32_t *,int32_t *,int32_t *);
void m17_fixed_lpc_response(const int32_t *,unsigned,unsigned,int32_t *,int32_t *);
