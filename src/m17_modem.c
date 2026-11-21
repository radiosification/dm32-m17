/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "m17_modem_internal.h"
#include "filter_taps.h"
_Static_assert((int64_t)M17_RX_TAPS_L1_BOUND*32768<INT32_MAX,"RX filter accumulator fits in 32 bits");
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
static bool eot_detected(const m17_demodulator *d,unsigned t) {
 const unsigned mask=M17_FILTER_HISTORY-1;
 /* The marker repeats every eight symbols. Require three copies before
  * ending a live stream. A cheap slope check skips most correlation work. */
 return d->rx->have_lsf && t>=230 &&
        d->filtered[t&mask]-d->filtered[(t-10)&mask]>=900 &&
        d->filtered[(t-80)&mask]-d->filtered[(t-90)&mask]>=900 &&
        d->filtered[(t-160)&mask]-d->filtered[(t-170)&mask]>=900 &&
        m17_sync_error(d,t,eot_symbols)<2048 &&
        m17_sync_error(d,t-80,eot_symbols)<2048 &&
        m17_sync_error(d,t-160,eot_symbols)<2048;
}
static void filtered(m17_demodulator *d,int16_t x) {
 unsigned t=d->ticks;d->filtered[t&(M17_FILTER_HISTORY-1)]=x;
 if(d->collecting) {
  if(t==d->next_symbol) {
   d->frame[d->frame_n++]=x;d->next_symbol+=10;
   if(d->frame_n==192) {
    /* Normalize using the sync's outer levels; reject bad/too-weak sync. */
    int32_t amp=0,offset=0,ps=0;
    const int8_t *pat=d->best_sync;
    for(unsigned k=0;k<8;k++) {amp+=d->frame[k]*pat[k];offset+=d->frame[k];ps+=pat[k];}
    amp=(8*amp-offset*ps)/(8*72-ps*ps);offset=(offset-amp*ps)/8;
    if(amp>=300) {
     for(unsigned k=0;k<192;k++)d->frame[k]=saturate((d->frame[k]-offset)*2048/amp);
     m17_receive_frame(d->rx,d->frame);
    }
    d->collecting=false;
   }
  }
 } else if(eot_detected(d,t)) {
  d->candidate=false;m17_receiver_end(d->rx);
 } else if(t>=80) {
  uint32_t a=m17_sync_error(d,t,lsf_sync_symbols),b=m17_sync_error(d,t,str_sync_symbols);
  uint32_t e=a<b?a:b;
  if(!d->candidate && e<2048) {d->candidate=true;d->candidate_end=t+9;d->best_error=UINT32_MAX;}
  if(d->candidate) {
   if(e<d->best_error) {d->best_error=e;d->best_tick=t;d->best_sync=a<b?lsf_sync_symbols:str_sync_symbols;}
   if(t==d->candidate_end) {
    d->candidate=false;d->collecting=true;d->frame_n=8;
    for(unsigned k=0;k<8;k++)d->frame[k]=d->filtered[(d->best_tick-70+10*k)&(M17_FILTER_HISTORY-1)];
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
   int16_t prev=d->filtered[(d->ticks-1)&(M17_FILTER_HISTORY-1)];
   filtered(d,(int16_t)(((int32_t)prev+y)/2));
   filtered(d,y);
  }
 }
}
