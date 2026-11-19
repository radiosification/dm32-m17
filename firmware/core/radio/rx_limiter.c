/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "rx_limiter.h"
#include <string.h>

static uint32_t magnitude(int32_t x) { return (uint32_t)(x<0?-x:x); }
static uint32_t clamp(uint32_t x,uint32_t lo,uint32_t hi)
{ return x<lo?lo:x>hi?hi:x; }
static uint32_t sqrt_ceiling(uint32_t n)
{
    uint32_t original=n,root=0,bit=1u<<30;
    while(bit>n)bit>>=2;
    while(bit) {
        if(n>=root+bit){n-=root+bit;root=(root>>1)+bit;}
        else root>>=1;
        bit>>=2;
    }
    return root*root<original?root+1:root;
}
void voice_rx_limiter_reset(voice_rx_limiter *s)
{
    memset(s,0,sizeof *s);
    s->gain_q15=32768;s->reference_peak=4096;s->reference_rms=1024;
    s->agc_gain_q15=32768;
}
void voice_rx_limiter_process(voice_rx_limiter *s,const int16_t in[160],int16_t out[160])
{
    uint64_t energy=0;uint32_t peak=0;
    for(unsigned i=0;i<160;i++) {
        uint32_t a=magnitude(in[i]);
        if(a>peak)peak=a;
        energy+=(uint64_t)a*a;
    }
    /* Round UP, so RMS rounding cannot permit an overshoot. */
    uint32_t rms=sqrt_ceiling((uint32_t)((energy+159)/160));
    uint32_t peak_limit=clamp(2*s->reference_peak,VOICE_RX_PEAK_MIN,VOICE_RX_PEAK_MAX);
    uint32_t rms_limit=clamp(2*s->reference_rms,VOICE_RX_RMS_MIN,VOICE_RX_RMS_MAX);
    uint32_t wanted=32768;
    if(peak>peak_limit)wanted=peak_limit*32768u/peak;
    if(rms>rms_limit) {
        uint32_t gain=rms_limit*32768u/rms;
        if(gain<wanted)wanted=gain;
    }
    /* Immediate block attack; at most 1/32 unity per 20 ms on release.
     * One common gain preserves the waveform instead of hard-clipping it.
     * An arbitrarily long bad tail cannot raise its own ceiling: only
     * unbounded-by-the-limiter, non-silent input updates the reference.
     */
    if(wanted<s->gain_q15)s->gain_q15=wanted;
    else {
        uint32_t release=wanted-s->gain_q15;
        s->gain_q15+=release>1024?1024:release;
    }
    /* Learn ONLY from ordinary, non-silent RAW decoder output. AGC never
     * observes its own limited output (which would cause gain pumping).
     * A burst or pause freezes both the AGC gain and level reference.
     */
    if(wanted==32768 && rms>=64 && peak>=256 &&
       rms<=2*s->reference_rms && peak<=2*s->reference_peak) {
        s->reference_peak=(uint32_t)((int32_t)s->reference_peak+((int32_t)peak-(int32_t)s->reference_peak)/16);
        s->reference_rms=(uint32_t)((int32_t)s->reference_rms+((int32_t)rms-(int32_t)s->reference_rms)/16);
        uint32_t desired=clamp(VOICE_RX_AGC_TARGET*32768u/s->reference_rms,8192,VOICE_RX_AGC_MAX_Q15);
        int32_t delta=(int32_t)desired-(int32_t)s->agc_gain_q15;
        s->agc_gain_q15=(uint32_t)((int32_t)s->agc_gain_q15+delta/(delta<0?4:32));
    }
    uint32_t gain=(uint32_t)((uint64_t)s->gain_q15*s->agc_gain_q15/32768);
    /* Translate the trusted RAW level into an OUTPUT ceiling using AGC,
     * then apply the absolute floors/caps. Applying the floor before boost
     * would allow very quiet voice's noise bursts to grow by the boost too.
     */
    uint32_t output_peak_limit=clamp((uint32_t)((uint64_t)2*s->reference_peak*s->agc_gain_q15/32768),
                                     VOICE_RX_PEAK_MIN,VOICE_RX_PEAK_MAX);
    uint32_t output_rms_limit=clamp((uint32_t)((uint64_t)2*s->reference_rms*s->agc_gain_q15/32768),
                                    VOICE_RX_RMS_MIN,VOICE_RX_RMS_MAX);
    if(peak && gain>output_peak_limit*32768u/peak)gain=output_peak_limit*32768u/peak;
    if(rms && gain>output_rms_limit*32768u/rms)gain=output_rms_limit*32768u/rms;
    if(gain<s->agc_gain_q15)s->limited_frames++;
    if(peak>s->input_peak)s->input_peak=peak;
    for(unsigned i=0;i<160;i++) {
        int32_t sample=(int32_t)((int64_t)in[i]*gain/32768);
        out[i]=(int16_t)sample;
        if(magnitude(sample)>s->output_peak)s->output_peak=magnitude(sample);
    }
    if(s->frames!=UINT32_MAX)s->frames++;
}
