/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "fixed_codec.h"
#include "codec2_mod.h"
#include "wide_math.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static codec2_encoder_t enc[2];
static codec2_decoder_t dec[2];
static uint32_t rng=1;
static uint32_t noise(void){rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;return rng;}
int main(void){
 for(unsigned n=0;n<10000;n++){
  int32_t a=(int32_t)noise(),b=(int32_t)noise();
  for(unsigned q=1;q<32;q++)assert((int32_t)m17_smul_shift(a,b,q)==(int32_t)(((int64_t)a*b)>>q));
 }
 for(unsigned i=0;i<2;i++){codec2_encoder_init(&enc[i]);codec2_decoder_init(&dec[i]);}
 int16_t in[160],out[2][160];uint8_t bits[2][8];
 for(unsigned n=0;n<500;n++){
  for(unsigned j=0;j<160;j++){
   int16_t x=(int16_t)noise();
   if(n<20)x=0;else if(n<40)x=32767;else if(n<60)x=-32768;else if(n<80)x=(j&1)?32767:-32768;
   else if(n<100)x=j?0:32767;else if(n<120)x=(int16_t)((int)(j*409)-32768);
   in[j]=x;
  }
  for(unsigned i=0;i<2;i++)codec2_encode(&enc[i],bits[i],in);
  assert(!memcmp(bits[0],bits[1],8));
  for(unsigned i=0;i<2;i++)codec2_decode(&dec[i],out[i],bits[0]);
  assert(!memcmp(out[0],out[1],sizeof out[0]));
 }
 /* On-air bit errors can produce every possible parameter index. */
 for(unsigned n=0;n<5000;n++){
  for(unsigned j=0;j<8;j++)bits[0][j]=(uint8_t)noise();
  for(unsigned i=0;i<2;i++)codec2_decode(&dec[i],out[i],bits[0]);
  assert(!memcmp(out[0],out[1],sizeof out[0]));
 }
 puts("PASS integer Codec2 full-scale/silence/noise/bit errors, resets, independent states and wide products");
 return 0;
}
