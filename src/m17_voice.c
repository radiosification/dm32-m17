/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "m17_voice.h"
static void voice_payload(void *ctx,const uint8_t payload[16],uint16_t fn) {
 m17_voice *v=ctx;int16_t pcm[160];(void)fn;
 for(unsigned i=0;i<2;i++) {
  codec2_decode(&v->codec.decoder,pcm,payload+8*i);
  if(v->speaker)v->speaker(v->ctx,pcm);
 }
}
void m17_voice_rx_begin(m17_voice *v) {
 v->transmitting=false;memset(&v->codec,0,sizeof v->codec);
 codec2_decoder_init(&v->codec.decoder);
 m17_receiver_init(&v->receiver,voice_payload,v);m17_demod_init(&v->demod,&v->receiver);
 m17_symbol_stream_init(&v->symbol_stream,&v->receiver);
}
void m17_voice_init(m17_voice *v,m17_speaker_sink speaker,void *ctx) {
 memset(v,0,sizeof *v);v->speaker=speaker;v->ctx=ctx;m17_voice_rx_begin(v);
}
int m17_voice_tx_begin(m17_voice *v,const char *src,const char *dst,unsigned can,int8_t symbols[192]) {
 if(can>15)return -1;
 if(set_LSF(&v->tx_lsf,src,dst,M17_TYPE_STREAM|M17_TYPE_VOICE|M17_TYPE_CAN(can),NULL))return -1;
 v->transmitting=true;v->seq=0;memset(&v->codec,0,sizeof v->codec);
 codec2_encoder_init(&v->codec.encoder);m17_mod_init(&v->mod);
 return gen_frame_i8(symbols,NULL,FRAME_LSF,&v->tx_lsf,0,0);
}
int m17_voice_tx(m17_voice *v,const int16_t speech[320],bool last,int8_t symbols[192]) {
 if(!v->transmitting)return -1;
 uint8_t payload[16];
 codec2_encode(&v->codec.encoder,payload,speech);
 codec2_encode(&v->codec.encoder,payload+8,speech+160);
 uint16_t seq=v->seq;v->seq=(seq+1)&0x7fff;
 int ret=gen_frame_i8(symbols,payload,FRAME_STR,&v->tx_lsf,seq%6,seq|(last?0x8000:0));
 if(last)v->transmitting=false;
 return ret;
}
int m17_voice_rx_phy(m17_voice *v,const uint8_t phy[48]) {
 if(v->transmitting)return -1;
 int16_t symbols[192];m17_unpack_phy(phy,symbols);return m17_receive_frame(&v->receiver,symbols);
}
void m17_voice_rx_phy_bytes(m17_voice *v,const uint8_t *phy,unsigned n) {
 if(!v->transmitting)m17_receive_phy_bytes(&v->symbol_stream,phy,n);
}
void m17_voice_rx_pcm(m17_voice *v,const int16_t *pcm,unsigned n) {
 if(!v->transmitting)m17_demodulate(&v->demod,pcm,n);
}
