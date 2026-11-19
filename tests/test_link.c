/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "m17_link.h"
#include "filter_taps.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
static m17_receiver receiver;
static m17_demodulator demod;
static m17_modulator mod;
static unsigned heard;
static uint8_t payload[32][16];
#ifdef M17_MODEM_TEST
extern uint32_t m17_test_sync_error(m17_demodulator *,const int8_t *);
static uint32_t reference_sync(const int16_t x[8],const int8_t *pat) {
 int32_t cross=0,mean=0,ps=0;
 for(unsigned k=0;k<8;k++){cross+=x[k]*pat[k];mean+=x[k];ps+=pat[k];assert(pat[k]==3||pat[k]==-3);}
 int32_t level=(8*cross-mean*ps)/(8*72-ps*ps);if(level<300)return UINT32_MAX;
 mean=(mean-level*ps)/8;uint64_t e=0;
 for(unsigned k=0;k<8;k++){int32_t d=(x[k]-mean)-pat[k]*level;e+=(uint64_t)((int64_t)d*d);}
 return (uint32_t)(e*1024u/(uint32_t)(level*level));
}
#endif
static void sink(void *ctx,const uint8_t p[16],uint16_t fn) {
 (void)ctx;unsigned n=fn&0x7fff;assert(n<32);assert(!memcmp(p,payload[n],16));heard++;
}
static void feed(const int16_t *pcm,unsigned n,unsigned noise) {
 static uint32_t seed=73;
 for(unsigned i=0;i<n;i++) {
  seed=seed*1664525+1013904223;
  int32_t x=pcm[i]+(noise?(int32_t)(seed%(noise*2+1))-(int32_t)noise:0);
  int16_t s=x>32767?32767:x<-32768?-32768:x;
  m17_demodulate(&demod,&s,1);
 }
}
static void reference_mod(const int8_t *symbols,int16_t pcm[320]) {
 static float ring[41];static unsigned at,ticks;
 unsigned n=0,j=0;
 for(unsigned t=0;t<960;t++,ticks++) {
  at=(at+1)%41;ring[at]=!(ticks%5)?symbols[j++]*2048*sqrtf(5):0;
  if(!(ticks%3)) {
   float x=0;for(unsigned k=0;k<41;k++)x+=rrc_taps_5[k]*ring[(at+41-k)%41];
   pcm[n++]=(int16_t)lrintf(x);
  }
 }
}
static float ring48[81];
static unsigned at48,ticks48;
static void reference_mod_48(const int8_t *symbols,int16_t pcm[320],unsigned phase) {
 unsigned n=0,j=0;
 for(unsigned t=0;t<1920;t++,ticks48++) {
  at48=(at48+1)%81;
  ring48[at48]=!(ticks48%10)?symbols[j++]*2048*sqrtf(10):0;
  if(ticks48%6==phase) {
   float x=0;for(unsigned k=0;k<81;k++)x+=rrc_taps_10[k]*ring48[(at48+81-k)%81];
   pcm[n++]=(int16_t)lrintf(x+1000);
  }
 }
 assert(n==320);
}
int main(void) {
 setbuf(stdout,NULL);
 /* Verify the optimized FIR against the original 64-bit scalar expression
  * on full-scale samples, impulses and noise, including ring wraparound. */
 static m17_receiver fir_rx;static m17_demodulator fir_demod;
 int16_t history[64]={0};unsigned pos=0;uint32_t random=1;
 m17_receiver_init(&fir_rx,NULL,NULL);m17_demod_init(&fir_demod,&fir_rx);
 for(unsigned i=0;i<10000;i++) {
  random=random*1664525u+1013904223u;
  int16_t x=i<128?(i&1?INT16_MIN:INT16_MAX):i<256?(i==128?INT16_MAX:0):(int16_t)(random>>16);
  pos=(pos+1)&63;history[pos]=x;uint32_t tick=fir_demod.ticks;
  int16_t previous=fir_demod.filtered[(tick-1)&127];
  m17_demodulate(&fir_demod,&x,1);
  for(unsigned phase=0;phase<3;phase++) {
   int64_t sum=0;for(unsigned k=phase;k<161;k+=3)sum+=(int32_t)rx_taps[k]*history[(pos-k/3)&63];
   int32_t y=(int32_t)(sum/16384);if(y>32767)y=32767;if(y<-32768)y=-32768;
   assert(fir_demod.filtered[(tick+2*phase+1)&127]==y);
   assert(fir_demod.filtered[(tick+2*phase)&127]==(previous+y)/2);
   previous=(int16_t)y;
  }
 }
 puts("PASS RX FIR bit-exact with 64-bit reference on 10000 full-scale samples");
#ifdef M17_MODEM_TEST
 unsigned accepted=0,rejected=0;
 for(unsigned test=0;test<100000;test++)for(unsigned pattern=0;pattern<2;pattern++) {
  const int8_t *pat=pattern?str_sync_symbols:lsf_sync_symbols;int16_t x[8];fir_demod.ticks=80;
  unsigned amplitude=300+test%10000;
  for(unsigned k=0;k<8;k++) {
   random=random*1664525u+1013904223u;
   int32_t v=test&1?(int16_t)(random>>16):(int32_t)pat[k]*amplitude+(int)(random%4001)-2000;
   if(v>32767)v=32767;if(v<-32768)v=-32768;
   x[k]=(int16_t)v;fir_demod.filtered[10+10*k]=x[k];
  }
  uint32_t expected=reference_sync(x,pat),actual=m17_test_sync_error(&fir_demod,pat);
  if(expected<2048){accepted++;assert(actual==expected);}else{rejected++;assert(actual>=2048);}
 }
 assert(accepted>1000&&rejected>1000);
 puts("PASS sync decisions match original 64-bit score on 200000 clean/noisy/clipped windows");
#endif
 lsf_t lsf;assert(set_LSF(&lsf,"M0TEST","@ALL",M17_TYPE_STREAM|M17_TYPE_VOICE,NULL)==0);
 for(unsigned n=0;n<32;n++)for(unsigned j=0;j<16;j++)payload[n][j]=(uint8_t)(n*19+j*7);
 int8_t symbols[192];int16_t s[192],pcm[320];uint8_t phy[48];
 /* Cross-check against the unmodified upstream float receive API. */
 gen_frame_i8(symbols,NULL,FRAME_LSF,&lsf,0,0);m17_pack_phy(symbols,phy);m17_unpack_phy(phy,s);
 for(unsigned j=0;j<192;j++)assert(s[j]==symbols[j]*2048);
 float symf[184];for(unsigned i=0;i<184;i++)symf[i]=s[i+8]/2048.f;
 lsf_t check;decode_LSF(&check,symf);assert(!memcmp(&lsf,&check,sizeof lsf));
 m17_receiver_init(&receiver,sink,NULL);assert(m17_receive_frame(&receiver,s)==0);assert(receiver.have_lsf);
 for(unsigned f=0;f<32;f++) {
  gen_frame_i8(symbols,payload[f],FRAME_STR,&lsf,f%6,f);
  m17_pack_phy(symbols,phy);m17_unpack_phy(phy,s);
  for(unsigned i=0;i<184;i++)symf[i]=s[i+8]/2048.f;
  uint8_t p[16],lich[5],lc;uint16_t fn;
  decode_str_frame(p,lich,&fn,&lc,symf);assert(fn==f&&!memcmp(p,payload[f],16));
  assert(m17_receive_frame(&receiver,s)==1);
 }
 assert(heard==32);puts("PASS upstream framing/FEC interop and native PHY packing");
 /* Every hard decision is correct, but uncertain soft observations have
  * a substantial nonzero path cost. They must not mute valid payloads. */
 heard=0;m17_receiver_init(&receiver,sink,NULL);
 gen_frame_i8(symbols,NULL,FRAME_LSF,&lsf,0,0);m17_pack_phy(symbols,phy);m17_unpack_phy(phy,s);
 assert(m17_receive_frame(&receiver,s)==0&&receiver.have_lsf);
 for(unsigned f=0;f<32;f++) {
  gen_frame_i8(symbols,payload[f],FRAME_STR,&lsf,f%6,f|(f==31?0x8000:0));
  for(unsigned i=0;i<192;i++)s[i]=symbols[i]*1800;
  assert(m17_receive_frame(&receiver,s)==1);
 }
 assert(heard==32&&receiver.ended&&!receiver.lost);
 puts("PASS uncertain soft observations retain all 32 bit-exact voice payloads");
 heard=0;m17_receiver_init(&receiver,sink,NULL);
 gen_frame_i8(symbols,NULL,FRAME_LSF,&lsf,0,0);m17_pack_phy(symbols,phy);m17_unpack_phy(phy,s);
 assert(m17_receive_frame(&receiver,s)==0&&receiver.have_lsf);
 gen_frame_i8(symbols,payload[0],FRAME_STR,&lsf,0,0);
 for(unsigned i=0;i<192;i++)s[i]=i<8?symbols[i]*2048:0;
 assert(m17_receive_frame(&receiver,s)<0&&!heard);
 puts("PASS erased payload stays muted after a valid link setup");
 /* A bank is 36 bytes, and frames are 48: boundaries cycle every 3 banks. */
 static uint8_t banks[34*48];
 static m17_symbol_stream native;
 uint32_t cnt=0;gen_preamble_i8(symbols,&cnt,PREAM_LSF);m17_pack_phy(symbols,banks);
 gen_frame_i8(symbols,NULL,FRAME_LSF,&lsf,0,0);m17_pack_phy(symbols,banks+48);
 for(unsigned f=0;f<32;f++) {
  gen_frame_i8(symbols,payload[f],FRAME_STR,&lsf,f%6,f|(f==31?0x8000:0));
  m17_pack_phy(symbols,banks+(f+2)*48);
 }
 for(unsigned chunk=1;chunk<=48;chunk++) {
  heard=0;m17_receiver_init(&receiver,sink,NULL);m17_symbol_stream_init(&native,&receiver);
  for(unsigned at=0;at<sizeof banks;at+=chunk) {
   unsigned n=sizeof banks-at;if(n>chunk)n=chunk;
   m17_receive_phy_bytes(&native,banks+at,n);
  }
  assert(heard==32&&receiver.ended&&!receiver.lost);
 }
 for(unsigned phase=0;phase<6;phase++) {
  memset(ring48,0,sizeof ring48);at48=ticks48=0;
  heard=0;m17_receiver_init(&receiver,sink,NULL);m17_demod_init(&demod,&receiver);
  uint32_t count=0;gen_preamble_i8(symbols,&count,PREAM_LSF);
  reference_mod_48(symbols,pcm,phase);feed(pcm,320,0);
  gen_frame_i8(symbols,NULL,FRAME_LSF,&lsf,0,0);
  reference_mod_48(symbols,pcm,phase);feed(pcm,320,0);
  for(unsigned f=0;f<32;f++) {
   gen_frame_i8(symbols,payload[f],FRAME_STR,&lsf,f%6,f|(f==31?0x8000:0));
   reference_mod_48(symbols,pcm,phase);feed(pcm,320,0);
  }
  count=0;gen_eot_i8(symbols,&count);
  reference_mod_48(symbols,pcm,phase);feed(pcm,320,0);
  assert(heard==32&&receiver.ended&&!receiver.lost);
 }
 puts("PASS independent 48 kHz RRC at all six ADC phases with DC offset");
 puts("PASS packed-symbol RX with every chunk size 1..48, including native 36-byte banks");
 for(unsigned reference=0;reference<2;reference++)for(unsigned noise=0;noise<=180;noise+=180) {
  heard=0;m17_receiver_init(&receiver,sink,NULL);m17_demod_init(&demod,&receiver);m17_mod_init(&mod);
  uint32_t count=0;gen_preamble_i8(symbols,&count,PREAM_LSF);
  if(reference)reference_mod(symbols,pcm);else m17_modulate(&mod,symbols,pcm);feed(pcm,320,noise);
  gen_frame_i8(symbols,NULL,FRAME_LSF,&lsf,0,0);
  if(reference)reference_mod(symbols,pcm);else m17_modulate(&mod,symbols,pcm);feed(pcm,320,noise);
  for(unsigned f=0;f<32;f++) {
   gen_frame_i8(symbols,payload[f],FRAME_STR,&lsf,f%6,f|(f==31?0x8000:0));
   if(reference)reference_mod(symbols,pcm);else m17_modulate(&mod,symbols,pcm);feed(pcm,320,noise);
  }
  count=0;gen_eot_i8(symbols,&count);
  if(reference)reference_mod(symbols,pcm);else m17_modulate(&mod,symbols,pcm);feed(pcm,320,noise);
  printf("8 kHz %s waveform noise=%u: heard=%u bad=%u lsf=%u lost=%u\n",reference?"upstream RRC":"integer TX",noise,heard,receiver.bad,receiver.lsf_count,receiver.lost);
  assert(heard==32&&receiver.ended&&!receiver.lost);
 }
 /* Late acquisition from LICH; reject encrypted and non-voice link types. */
 heard=0;m17_receiver_init(&receiver,sink,NULL);
 for(unsigned f=0;f<12;f++) {
  gen_frame_i8(symbols,payload[f],FRAME_STR,&lsf,f%6,f);
  m17_pack_phy(symbols,phy);m17_unpack_phy(phy,s);m17_receive_frame(&receiver,s);
 }
 assert(receiver.have_lsf&&heard==7);
 set_LSF(&lsf,"M0TEST","@ALL",M17_TYPE_STREAM|M17_TYPE_VOICE|M17_TYPE_ENCR_AES,NULL);
 gen_frame_i8(symbols,NULL,FRAME_LSF,&lsf,0,0);m17_pack_phy(symbols,phy);m17_unpack_phy(phy,s);
 m17_receiver_init(&receiver,sink,NULL);assert(m17_receive_frame(&receiver,s)<0&&!receiver.have_lsf);
 puts("PASS late entry, call end, type/cipher filtering; 8 kHz clean/noisy independent waveforms");
 return 0;
}
