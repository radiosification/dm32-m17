/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Radio task owns hardware; m17_worker owns codec; callbacks only move PCM.
 * No vocoder, flash access or RTOS waits in audio/IRQ callbacks. */
#include "radio_m17.h"
#include "config.h"
#if CONFIG_M17
#include "board.h"
#include "hal_time.h"
#include "FreeRTOS.h"
#include "task.h"
#include "settings/settings.h"
#include "codeplug/codeplug.h"
#include "m17_voice.h"
#include "mic_audio.h"
#include "radio/radio.h"
#include "radio/beep.h"
#include "radio_dv_reasons.h"
#include <string.h>

typedef struct {int16_t data[2048];volatile uint32_t head,tail;} ring;
static ring m17_input,m17_output,m17_wave,m17_microphone;
static m17_voice voice;
static radio_dv_cfg_t m17_channel;
static TaskHandle_t m17_worker;
static volatile uint32_t m17_epoch,m17_ready,m17_worker_epoch,m17_done,m17_frames,m17_last_ms,m17_overruns,m17_underruns;
static volatile bool m17_selected,m17_want_tx,m17_ptt,m17_failed;
static bool m17_keyed,m17_draining,m17_audio_on;
static volatile bool m17_rx_ended;

/* Buffer two 40 ms RF frames before starting or resuming playback. */
enum { RX_BLOCK = 160, RX_FRAME = 2 * RX_BLOCK, RX_PREFILL = 2 * RX_FRAME,
       RX_GAP_HOLD_MS = 160 };
static uint32_t m17_drain_at,m17_calls;
static uint8_t m17_last_reason;
static m17_mic_audio mic_dsp;
static m17_mic_audio_options mic_options;
static char m17_source[10],m17_received_source[10],m17_received_destination[10];
static uint8_t m17_received_can;
static volatile uint32_t m17_seen,m17_seen_ms,m17_rx_max_us,m17_tx_max_us;
static unsigned size(ring *r){return r->head-r->tail;}
static void clear(ring *r){r->head=r->tail=0;}
static bool push(ring *r,const int16_t *s,unsigned n) {
 bool ok=false;taskENTER_CRITICAL();
 if((r==&m17_output||r==&m17_wave)&&m17_worker_epoch!=m17_epoch){taskEXIT_CRITICAL();return true;}
 if(size(r)+n<=2048){for(unsigned i=0;i<n;i++)r->data[(r->head+i)&2047]=s[i];r->head+=n;ok=true;}
 taskEXIT_CRITICAL();return ok;
}
static unsigned pop(ring *r,int16_t *s,unsigned n) {
 taskENTER_CRITICAL();unsigned k=size(r);if(k>n)k=n;
 for(unsigned i=0;i<k;i++){s[i]=r->data[(r->tail+i)&2047];}
 r->tail+=k;
 taskEXIT_CRITICAL();return k;
}
static void audio(const int16_t *mic,int16_t *spk,void *ctx)
{
 (void)ctx;
 static uint32_t playback_epoch;
 static bool started, buffering;
 memset(spk,0,RX_BLOCK*sizeof(*spk));
 if(!m17_selected)return;
 if(playback_epoch!=m17_epoch) {
  playback_epoch=m17_epoch;
  started=false;
  buffering=true;
 }
 if(m17_want_tx) {
  if(mic&&m17_ptt&&!push(&m17_microphone,mic,RX_BLOCK)){m17_overruns++;m17_failed=true;}
  return;
 }

 unsigned queued=size(&m17_output);
 bool recent=m17_last_ms&&hal_millis()-m17_last_ms<RX_GAP_HOLD_MS;
 bool active=recent&&!m17_rx_ended;
 unsigned n=0;
 if(!buffering||queued>=RX_PREFILL) {
  n=pop(&m17_output,spk,RX_BLOCK);
 } else if(!active&&queued) {
  /* An underfilled call must not leave samples for the next caller. */
  taskENTER_CRITICAL();
  m17_output.tail=m17_output.head;
  taskEXIT_CRITICAL();
 }
 if(n)started=true;
 buffering=n<RX_BLOCK;
 if(buffering&&started&&active)m17_underruns++;
 bool tone=beep_mix(spk);
 bool play=n||tone||(started&&active)||
           (board.audio->spk_pending&&board.audio->spk_pending());
 if(!active&&!n)started=false;
 if(play!=m17_audio_on) {
  m17_audio_on=play;
  board.audio->speaker_pcm(play);
 }
}
/* IRQ context, single consumer; m17_worker publishes head only after copying. */
static void pull(int16_t *pcm,uint16_t n) {
 unsigned available=m17_wave.head-m17_wave.tail;
 for(unsigned i=0;i<n;i++)pcm[i]=i<available?m17_wave.data[(m17_wave.tail+i)&2047]:0;
 m17_wave.tail+=available<n?available:n;
 /* A gap changes the RF symbol clock. Stop this call through the higher
  * priority radio task instead of repeatedly sending malformed frames. */
 if(available<n && !m17_done){m17_underruns++;m17_failed=true;}
}
/* Called before decoding a payload as well as before queueing playback. */
static bool radio_m17_admit(const lsf_t *lsf) {
 unsigned type=((unsigned)lsf->type[0]<<8)|lsf->type[1];
 return m17_selected&&!m17_want_tx&&m17_worker_epoch==m17_epoch&&
        (m17_channel.hear_all||m17_channel.nac_rx==0xffff||((type>>7)&15)==m17_channel.nac_rx);
}
static void speaker(void *ctx,const int16_t pcm[160]) {
 (void)ctx;if(!radio_m17_admit(&voice.receiver.lsf))return;
 taskENTER_CRITICAL();
 if(m17_worker_epoch!=m17_epoch){taskEXIT_CRITICAL();return;}
 if(!push(&m17_output,pcm,RX_BLOCK))m17_overruns++;
 m17_frames++;
 m17_last_ms=hal_millis();
 m17_rx_ended=false;
 decode_callsign_bytes(m17_received_source,voice.receiver.lsf.src);
 decode_callsign_bytes(m17_received_destination,voice.receiver.lsf.dst);
 m17_received_can=(((unsigned)voice.receiver.lsf.type[0]<<8)|voice.receiver.lsf.type[1])>>7&15;
 taskEXIT_CRITICAL();
}
static void modulate(const int8_t s[192]) {
 int16_t pcm[320];m17_modulate(&voice.mod,s,pcm);
 /* M17 inner deviation is 800 Hz; the HAL level denotes 1500 Hz. */
 int32_t nominal=board.audio_tap->fm_tx_nominal_peak;
 if(nominal<=0)nominal=1375;
 int32_t scale=nominal*800*32/1500;
 for(unsigned i=0;i<320;i++)pcm[i]=(int16_t)((int32_t)pcm[i]*scale/65536);
 if(m17_worker_epoch==m17_epoch&&m17_selected&&!push(&m17_wave,pcm,320))m17_overruns++;
}
static void run(void *ctx) {
 (void)ctx;uint32_t seen=0;
 for(;;) {
  if(seen!=m17_epoch) {
   seen=m17_epoch;m17_worker_epoch=seen;
   if(m17_selected) {
    if(m17_want_tx) {
     taskENTER_CRITICAL();m17_mic_audio_options options=mic_options;taskEXIT_CRITICAL();m17_mic_audio_reset(&mic_dsp,options);
     int8_t symbols[192];
     if(m17_voice_tx_begin(&voice,m17_source,"@ALL",m17_channel.nac_tx&15,symbols))m17_failed=true;
     else {
      int8_t preamble[192];uint32_t count=0;gen_preamble_i8(preamble,&count,PREAM_LSF);
      modulate(preamble);modulate(symbols);
     }
    } else {
     m17_voice_init(&voice,speaker,NULL);
     voice.admit=radio_m17_admit;
    }
   }
   if(seen==m17_epoch)m17_ready=seen;
  }
  if(m17_selected&&seen==m17_epoch&&m17_ready==seen&&!m17_failed) {
   if(m17_want_tx&&!m17_done&&2048-size(&m17_wave)>=640) {
    int16_t pcm[320];unsigned n=0;
    if(size(&m17_microphone)>=320||!m17_ptt) {
     n=pop(&m17_microphone,pcm,320);memset(pcm+n,0,(320-n)*sizeof(int16_t));
     uint32_t began=hal_micros();
     m17_mic_audio_process(&mic_dsp,pcm);
     m17_mic_audio_process(&mic_dsp,pcm+160);
     int8_t symbols[192];bool last=!m17_ptt&&size(&m17_microphone)==0;
     if(!m17_voice_tx(&voice,pcm,last,symbols))modulate(symbols);else m17_failed=true;
     uint32_t elapsed=hal_micros()-began;if(elapsed>m17_tx_max_us)m17_tx_max_us=elapsed;
     if(last){uint32_t count=0;gen_eot_i8(symbols,&count);modulate(symbols);m17_done=seen;}
    }
   } else if(!m17_want_tx) {
    int16_t pcm[160];unsigned n=pop(&m17_input,pcm,160);
    if(n){uint32_t began=hal_micros();m17_voice_rx_pcm(&voice,pcm,n);
     if(seen==m17_epoch)m17_rx_ended=voice.receiver.ended;
     uint32_t elapsed=hal_micros()-began;if(elapsed>m17_rx_max_us)m17_rx_max_us=elapsed;
     if(m17_seen!=voice.receiver.frames){m17_seen=voice.receiver.frames;m17_seen_ms=hal_millis();
      taskENTER_CRITICAL();decode_callsign_bytes(m17_received_source,voice.receiver.lsf.src);decode_callsign_bytes(m17_received_destination,voice.receiver.lsf.dst);m17_received_can=(((unsigned)voice.receiver.lsf.type[0]<<8)|voice.receiver.lsf.type[1])>>7&15;taskEXIT_CRITICAL();}
    }
   }
  }
  vTaskDelay(pdMS_TO_TICKS(1));
 }
}
static bool identity(m17_mic_audio_options *options) {
 static settings_t settings;settings_read(&settings);
 if(options) {
  options->gain_step=settings.mic_gain;
  options->agc=(settings.accent & SETTINGS_AUDIO_AGC)!=0;
 }
 char name[CODEPLUG_NAME_LEN+1];codeplug_radio_name(name);
 const char *s=name[0]?name:settings.radio_name;
 unsigned n=0;while(n<CODEPLUG_RADIO_NAME_LEN&&s[n]){m17_source[n]=s[n]>='a'&&s[n]<='z'?s[n]-32:s[n];n++;}m17_source[n]=0;
 uint8_t address[6];return n&&s[n]==0&&m17_source[0]!='@'&&encode_callsign_bytes(address,m17_source)==0;
}
static void reset(bool tx) {
 taskENTER_CRITICAL();clear(&m17_input);clear(&m17_output);clear(&m17_wave);clear(&m17_microphone);
 memset(m17_received_source,0,10);memset(m17_received_destination,0,10);m17_received_can=0;
 m17_ready=m17_done=0;m17_seen=m17_seen_ms=m17_last_ms=0;m17_want_tx=tx;m17_rx_ended=false;m17_failed=false;m17_epoch++;taskEXIT_CRITICAL();
}
static void rx(void) {
 if(board.audio->mic_capture)board.audio->mic_capture(false);
 if(!board.radio->rx(m17_channel.rx_hz,RADIO_BW_12K5,NULL)||!board.audio_tap->fm_rx_flat(true))m17_failed=true;
}
bool radio_m17_select(const radio_dv_cfg_t *cfg) {
 if(m17_selected) {
  m17_selected=false;m17_epoch++;board.audio_tap->fm_rx_flat(false);
  if(m17_keyed){board.audio_tap->fm_tx_inject(NULL,AUDIO_TAP_FLAT);board.radio->tx_off();}
  board.audio->speaker_pcm(false);board.audio->stop();m17_keyed=m17_audio_on=m17_draining=false;
 }
 if(!cfg)return true;
 if(!board.audio||!board.audio_tap||!board.audio_tap->fm_rx_flat||!board.audio_tap->fm_tx_inject)return false;
 if(!m17_worker&&xTaskCreate(run,"m17",4096,NULL,3,&m17_worker)!=pdPASS)return false;
 m17_channel=*cfg;m17_selected=true;m17_ptt=false;reset(false);rx();
 if(!board.audio->start(audio,NULL)){m17_selected=false;board.audio_tap->fm_rx_flat(false);return false;}
 if(board.audio->mic_capture)board.audio->mic_capture(false);
 return !m17_failed;
}
bool radio_m17_identity_valid(void){return identity(NULL);}
bool radio_m17_ptt(bool down) {
 if(!m17_selected)return false;
 if(down) {
  m17_mic_audio_options options;
  if(m17_want_tx||!identity(&options))return false;
  taskENTER_CRITICAL();mic_options=options;taskEXIT_CRITICAL();
  board.audio_tap->fm_rx_flat(false);board.audio->speaker_pcm(false);m17_audio_on=false;
  m17_ptt=true;reset(true);
  if(board.audio->mic_capture)board.audio->mic_capture(true);
 } else m17_ptt=false;
 return true;
}
void radio_m17_service(uint32_t now) {
 if(!m17_selected)return;
 if(m17_want_tx) {
  if(!m17_keyed&&!m17_ptt){reset(false);rx();return;}
  if(m17_failed){if(m17_keyed){board.audio_tap->fm_tx_inject(NULL,AUDIO_TAP_FLAT);board.radio->tx_off();}m17_keyed=false;m17_ptt=false;m17_last_reason=NT_IO;m17_calls++;reset(false);rx();return;}
  /* Include two voice frames in addition to preamble/LSF before keying.
   * Cold-start codec work must not consume the entire modulation reserve. */
  if(!m17_keyed&&m17_ready==m17_epoch&&size(&m17_wave)>=1280) {
   if(!board.radio->tx(m17_channel.tx_hz,RADIO_BW_12K5,m17_channel.power,NULL)||!board.audio_tap->fm_tx_inject(pull,AUDIO_TAP_FLAT)) {
    board.radio->tx_off();m17_failed=true;return;
   }
   m17_keyed=true;m17_draining=false;
  }
  if(m17_keyed&&m17_done==m17_epoch&&!size(&m17_wave)) {
   if(!m17_draining){m17_draining=true;m17_drain_at=now;}
   else if(now-m17_drain_at>=64){board.audio_tap->fm_tx_inject(NULL,AUDIO_TAP_FLAT);board.radio->tx_off();m17_keyed=m17_draining=false;m17_last_reason=NT_TX_COMPLETE;m17_calls++;reset(false);rx();}
  }
 } else if(m17_ready==m17_epoch) {
  int16_t pcm[160];for(unsigned i=0;i<8;i++){unsigned n=board.audio_tap->fm_rx_read(pcm,160);if(!n)break;if(!push(&m17_input,pcm,n))m17_overruns++;}

 }
}
void radio_m17_status(radio_dv_status_t *s,uint32_t now) {
 s->active=m17_selected;s->fault=m17_failed;s->transmitting=m17_want_tx;s->keyed=m17_keyed;s->tx_calls=m17_calls;
 s->sync=m17_seen_ms&&now-m17_seen_ms<400;s->sync_ours=s->sync&&(m17_channel.hear_all||m17_channel.nac_rx==0xffff||m17_channel.nac_rx==m17_received_can);s->playing=m17_last_ms&&now-m17_last_ms<80&&m17_audio_on;
 taskENTER_CRITICAL();memcpy(s->callsign,m17_want_tx?m17_source:m17_received_source,10);memcpy(s->destination,m17_want_tx?"@ALL\0\0\0\0\0":m17_received_destination,10);taskEXIT_CRITICAL();
 s->nac=m17_received_can;s->tx_reason=m17_last_reason;s->tx_blocks=m17_frames;
}
void radio_m17_counters(uint32_t v[6]){taskENTER_CRITICAL();v[0]=m17_frames;v[1]=m17_overruns;v[2]=m17_underruns;v[3]=m17_rx_max_us;v[4]=m17_tx_max_us;v[5]=m17_epoch;taskEXIT_CRITICAL();}

void radio_m17_rx_counters(uint32_t *good,uint32_t *bad,uint32_t *lost)
{
 taskENTER_CRITICAL();
 *good=voice.receiver.frames;
 *bad=voice.receiver.bad;
 *lost=voice.receiver.lost;
 taskEXIT_CRITICAL();
}
#endif
