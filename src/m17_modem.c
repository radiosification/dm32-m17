/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "m17_link.h"
#include "filter_taps.h"
_Static_assert((int64_t)M17_RX_TAPS_L1_BOUND*32768<INT32_MAX,"RX filter accumulator fits in 32 bits");
#ifdef MODEM_DEBUG
#include <stdio.h>
#endif
static int16_t saturate(int32_t x) {return x>32767?32767:x<-32768?-32768:x;}
void m17_mod_init(m17_modulator *m) {memset(m,0,sizeof *m);}
void m17_modulate(m17_modulator *m,const int8_t symbols[192],int16_t pcm[320]) {
 unsigned sym=0,n=0;
 for(unsigned t=0;t<960;t++) {
  unsigned phase=m->tick%5;
  if(!phase) {m->pos=(m->pos+1)%9;m->symbols[m->pos]=symbols[sym++];}
  if(!(m->tick%3)) {
   int32_t sum=0;
   for(unsigned k=phase;k<41;k+=5)sum+=tx_taps[k]*m->symbols[(m->pos+9-k/5)%9];
   pcm[n++]=saturate(sum/8); /* 2048 LSB per nominal level. */
  }
  m->tick++;
 }
}
void m17_demod_init(m17_demodulator *d,m17_receiver *r) {memset(d,0,sizeof *d);d->rx=r;}
static uint32_t sync_error(m17_demodulator *d,const int8_t *pat) {
 int32_t cross=0,mean=0,ps=0;int16_t x[8];
 for(unsigned k=0;k<8;k++) {x[k]=d->filtered[(d->ticks-70+10*k)&127];mean+=x[k];cross+=x[k]*pat[k];ps+=pat[k];}
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
#ifdef M17_MODEM_TEST
uint32_t m17_test_sync_error(m17_demodulator *d,const int8_t *pat){return sync_error(d,pat);}
#endif
static void filtered(m17_demodulator *d,int16_t x) {
 unsigned t=d->ticks;d->filtered[t&127]=x;
 if(d->collecting) {
  if(t==d->next_symbol) {
   d->frame[d->frame_n++]=x;d->next_symbol+=10;
   if(d->frame_n==192) {
    /* Normalize using the sync's outer levels; reject bad/too-weak sync. */
    int32_t amp=0,offset=0,ps=0;
    const int8_t *pat=d->frame[0]>0?lsf_sync_symbols:str_sync_symbols;
    for(unsigned k=0;k<8;k++) {amp+=d->frame[k]*pat[k];offset+=d->frame[k];ps+=pat[k];}
    amp=(8*amp-offset*ps)/(8*72-ps*ps);offset=(offset-amp*ps)/8;
    if(amp>=300) {
     for(unsigned k=0;k<192;k++)d->frame[k]=saturate((d->frame[k]-offset)*2048/amp);
     
#ifdef MODEM_DEBUG
     fprintf(stderr,"frame t=%u amp=%d ret=",t,amp);
     int ret=m17_receive_frame(d->rx,d->frame);fprintf(stderr,"%d\n",ret);
#else
     m17_receive_frame(d->rx,d->frame);
#endif
    }
    d->collecting=false;
   }
  }
 } else if(t>=80) {
  uint32_t a=sync_error(d,lsf_sync_symbols),b=sync_error(d,str_sync_symbols);
  uint32_t e=a<b?a:b;
#ifdef MODEM_DEBUG
  if(e<2048)fprintf(stderr,"sync t=%u a=%u b=%u candidate=%d\n",t,a,b,d->candidate);
#endif
  if(!d->candidate && e<2048) {d->candidate=true;d->candidate_end=t+9;d->best_error=UINT32_MAX;}
  if(d->candidate) {
   if(e<d->best_error) {d->best_error=e;d->best_tick=t;}
   if(t==d->candidate_end) {
    d->candidate=false;d->collecting=true;d->frame_n=8;
    for(unsigned k=0;k<8;k++)d->frame[k]=d->filtered[(d->best_tick-70+10*k)&127];
    d->next_symbol=d->best_tick+10;
   }
  }
 }
 d->ticks++;
}
void m17_demodulate(m17_demodulator *d,const int16_t *pcm,unsigned n) {
 for(unsigned i=0;i<n;i++) {
  d->pos=(d->pos+1)&63;d->samples[d->pos]=d->samples[d->pos+64]=pcm[i];
  const int16_t *sample=&d->samples[d->pos+64];
  /* Three 24 kHz outputs per 8 kHz input; merged interpolation + RX RRC. */
  for(unsigned phase=0;phase<3;phase++) {
   int32_t sum=0;
   for(unsigned k=0;k<54;k++)sum+=(int32_t)rx_polyphase[phase][k]*sample[-(int)k];
   /* Half-step timing candidates without doubling the FIR work. The
    * matched filter remains at 24 kHz; its midpoint and current sample
    * give acquisition and frame sampling ten positions per symbol. */
   int16_t y=saturate(sum/16384);
   int16_t prev=d->filtered[(d->ticks-1)&127];
   filtered(d,(int16_t)(((int32_t)prev+y)/2));
   filtered(d,y);
  }
 }
}
