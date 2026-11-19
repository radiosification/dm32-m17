/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "m17_link.h"
#include <limits.h>
static int32_t clamp(int32_t x,int32_t lo,int32_t hi) {return x<lo?lo:x>hi?hi:x;}
/* Integer slicer, output equivalent to libm17's +/-1,+/-3 convention.
 * Input units: 2048 per symbol level. Keep FEC soft metrics integer. */
static void soft_bits(uint16_t out[368],const int16_t s[184]) {
 for(unsigned i=0;i<184;i++) {
  int32_t x=s[i];
  uint16_t b1=(uint16_t)clamp(32767-x*16,0,65535);
  uint16_t b0=(uint16_t)clamp((x<0?-x:x)*16-32768,0,65535);
  unsigned r=(uint8_t)(rand_seq[i/4]<<(2*(i%4)));
  if(r&128)b1=65535-b1;
  if(r&64)b0=65535-b0;
  out[intrl_seq[2*i]]=b1;out[intrl_seq[2*i+1]]=b0;
 }
}
static bool good_lsf(const lsf_t *l) {
 unsigned type=((unsigned)l->type[0]<<8)|l->type[1];
 return CRC_M17((const uint8_t *)l,30)==0 && (type&7)==(M17_TYPE_STREAM|M17_TYPE_VOICE) && !(type&(3u<<3));
}
static bool sync_matches(const int16_t *s,const int8_t *pattern) {
 for(unsigned i=0;i<8;i++)if((s[i]>=0)!=(pattern[i]>=0))return false;
 return true;
}
void m17_receiver_init(m17_receiver *r,m17_payload_sink sink,void *ctx) {
 memset(r,0,sizeof *r);r->sink=sink;r->ctx=ctx;
}
int m17_receive_frame(m17_receiver *r,const int16_t s[192]) {
 uint16_t soft[368];uint8_t data[31];
 if(sync_matches(s,eot_symbols)) {r->ended=true;r->have_lsf=false;return 0;}
 bool lsf=sync_matches(s,lsf_sync_symbols),stream=sync_matches(s,str_sync_symbols);
 if(!lsf&&!stream) {r->bad++;return -1;}
 soft_bits(soft,s+8);
 if(lsf) {
  viterbi_decode_punctured_ctx(&r->fec,data,soft,puncture_pattern_1,368,61);
  lsf_t l;memcpy(&l,data+1,30);
  if(!good_lsf(&l)) {r->have_lsf=false;r->lich_mask=0;r->bad++;return -1;}
  r->lsf=l;r->have_lsf=true;r->ended=false;r->have_fn=false;r->lich_mask=0;r->lsf_count++;
  return 0;
 }
 uint8_t lich[6];
 if(decode_LICH(lich,soft)==0 && (lich[5]>>5)<6) {
  unsigned idx=lich[5]>>5;
  memcpy((uint8_t *)&r->partial+idx*5,lich,5);r->lich_mask|=1u<<idx;
  if(r->lich_mask==63 && good_lsf(&r->partial) && !r->have_lsf) {
   r->lsf=r->partial;r->have_lsf=true;r->ended=false;r->lsf_count++;
  }
 }
 uint32_t cost=viterbi_decode_punctured_ctx(&r->fec,data,soft+96,puncture_pattern_2,272,12);
 /* Soft observations carry an unavoidable cost even when every hard bit
  * agrees with the decoded path. Limit corrections above that floor, not
  * uncertainty in otherwise valid voice frames. */
 uint32_t floor=0;
 for(unsigned i=96;i<368;i++) {
  unsigned x=soft[i];floor+=x<32768?x:65535u-x;
 }
 /* Keep at least 75% mean soft-decision certainty: subtracting the floor
  * alone must not admit an erased payload whose paths all look alike. */
 if(floor>65535u*272u/8u || cost>floor+65535u*12) {r->bad++;return -1;}
 uint16_t fn=((uint16_t)data[1]<<8)|data[2],seq=fn&0x7fff;
 if(!r->have_lsf || r->ended)return 0;
 if(r->have_fn) {
  unsigned delta=(seq-r->last_fn)&0x7fff;
  if(!delta || delta>0x4000) {r->duplicates++;return 0;}
  r->lost+=delta-1;
 }
 r->last_fn=seq;r->have_fn=true;r->frames++;
 if(r->sink)r->sink(r->ctx,data+3,fn);
 if(fn&0x8000) {r->ended=true;r->have_lsf=false;}
 return 1;
}
void m17_pack_phy(const int8_t s[192],uint8_t out[48]) {
 memset(out,0,48);
 for(unsigned i=0;i<192;i++) {
  unsigned dib=s[i]>0?(s[i]>1?1:0):(s[i]<-1?3:2);
  out[i/4]|=dib<<(6-2*(i%4));
 }
}
void m17_unpack_phy(const uint8_t in[48],int16_t s[192]) {
 for(unsigned i=0;i<192;i++)s[i]=symbol_map[(in[i/4]>>(6-2*(i%4)))&3]*2048;
}
void m17_symbol_stream_init(m17_symbol_stream *s,m17_receiver *rx) {
 memset(s,0,sizeof *s);s->rx=rx;
}
void m17_receive_phy_bytes(m17_symbol_stream *s,const uint8_t *bytes,unsigned n) {
 for(unsigned i=0;i<n;i++)for(unsigned j=0;j<4;j++) {
  int16_t x=symbol_map[(bytes[i]>>(6-j*2))&3]*2048;
  if(s->collecting) {
   s->frame[s->count++]=x;
   if(s->count==192) {m17_receive_frame(s->rx,s->frame);s->collecting=false;s->history_count=0;}
  } else {
   memmove(s->history,s->history+1,7*sizeof(int16_t));s->history[7]=x;
   if(s->history_count<8)s->history_count++;
   if(s->history_count==8 && (sync_matches(s->history,lsf_sync_symbols)||sync_matches(s->history,str_sync_symbols))) {
    memcpy(s->frame,s->history,sizeof s->history);s->count=8;s->collecting=true;
   }
  }
 }
}
