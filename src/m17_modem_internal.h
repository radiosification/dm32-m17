/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "m17_link.h"

static inline uint32_t m17_sync_error(const m17_demodulator *d,unsigned t,const int8_t *pat) {
 int32_t cross=0,mean=0,ps=0;int16_t x[8];
 for(unsigned k=0;k<8;k++) {x[k]=d->filtered[(t-70+10*k)&(M17_FILTER_HISTORY-1)];mean+=x[k];cross+=x[k]*pat[k];ps+=pat[k];}
 int32_t level=(8*cross-mean*ps)/(8*72-ps*ps);
 /* Sync symbols are +/-3: any int16 window gives level <= 10923.
  * Retain fractional error so adjacent timing candidates can be ranked. */
 if(level<300||level>10923)return UINT32_MAX;
 mean=(mean-level*ps)/8;
 uint32_t den=(uint32_t)level*level,limit=2*den,e=0;
 for(unsigned k=0;k<8;k++) {
  int32_t diff=(x[k]-mean)-pat[k]*level;unsigned a=diff<0?-diff:diff;
  if(a>=(unsigned)level*2)return UINT32_MAX;
  uint32_t square=a*a;
  if(square>=limit-e)return UINT32_MAX;
  e+=square;
 }
 return (uint32_t)((uint64_t)e*1024u/den);
}
