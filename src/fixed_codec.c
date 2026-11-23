/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Codec2 3200 integer encoder and decoder, derived from the pinned M17 Codec2-mod path.
 * LSP/pitch use turns in Q32, LPC coefficients Q22, amplitudes/PCM Q8,
 * unit phasors Q15 and log2 powers Q16. Envelope powers share a per-frame
 * scale to preserve formant peaks without per-operation soft floats. */
#include "codec2_mod.h"
#include "quantise.h"
#include "fixed_codec.h"
#include "fixed_tables.h"
#include "wide_math.h"
#include <string.h>
#include <limits.h>
#include <stdbool.h>

typedef struct __attribute__((may_alias)) {
 uint32_t random,phase,wo,lsp[10],last_pcm_peak;
 int32_t energy,bg,overlap[160];
 int voiced;
} fixed_decoder;
_Static_assert(sizeof(fixed_decoder)<=sizeof(codec2_decoder_t),"decoder storage");
static int32_t mul(int32_t a,int32_t b,unsigned q) {
 return (int32_t)m17_smul_shift(a,b,q);
}
static int32_t cosine(uint32_t phase) {
 unsigned at=phase>>20,f=(phase>>4)&65535;
 int32_t a=c2_cos[at],delta=c2_cos[at+1]-a;
 return a+mul(delta,(int32_t)f,16);
}
static int32_t log2q(uint32_t x) {
 if(!x)return -40*65536;
 unsigned e=31-__builtin_clz(x),m=(x<<(31-e))&0x7fffffff;
 unsigned at=m>>23,f=(m>>7)&65535;
 return (int32_t)e*65536+c2_log[at]+((c2_log[at+1]-c2_log[at])*(int32_t)f>>16);
}
static uint32_t exp2q(int32_t x) {
 int e=x>>16;unsigned f=(uint32_t)x&65535,at=f>>8;
 uint32_t m=c2_exp[at]+((c2_exp[at+1]-c2_exp[at])*(f&255)>>8);
 if(e>=31)return INT32_MAX;
 if(e>=24)return m<<(e-24);
 if(e< -7)return 0;
 return m>>(24-e);
}
static int32_t logadd(int32_t a,int32_t b) {
 if(a<b){int32_t t=a;a=b;b=t;}
 unsigned d=(unsigned)(a-b);if(d>=16*65536)return a;
 unsigned at=d>>11,f=d&2047;
 return a+c2_logadd[at]+((c2_logadd[at+1]-c2_logadd[at])*(int32_t)f>>11);
}
static uint32_t root(uint32_t x) {
 if(!x)return 0;
 uint32_t y=1u<<((32-__builtin_clz(x)+1)/2);
 for(;;){uint32_t z=(y+x/y)/2;if(z>=y)return y;y=z;}
}
static uint32_t random_phase(fixed_decoder *s) {
 s->random=s->random*1103515245u+12345u;
 unsigned r=(s->random>>16)&32767;
 return r*131076u; /* 2^32/32767, rounded; same Codec2 PRNG sequence. */
}
static void lsp_lpc(const uint32_t *lsp,int32_t ak[11]) {
 int32_t freq[10],w[22]={0};
 for(unsigned i=0;i<10;i++)freq[i]=cosine(lsp[i]);
 int32_t x1=1<<22,x2=1<<22;
 for(unsigned j=0;j<=10;j++) {
  for(unsigned i=0;i<5;i++) {
   int32_t *p=w+4*i;
   int32_t y1=x1-mul(freq[2*i],p[0],29)+p[1];
   int32_t y2=x2-mul(freq[2*i+1],p[2],29)+p[3];
   p[1]=p[0];p[3]=p[2];p[0]=x1;p[2]=x2;x1=y1;x2=y2;
  }
  ak[j]=(x1+w[20])/2+(x2-w[21])/2;
  w[20]=x1;w[21]=x2;x1=x2=0;
 }
}
static void spectrum(const int32_t ak[11],int32_t re[257],int32_t im[257],int32_t power[256]) {
 uint32_t peak=1;int32_t q[11];
 for(unsigned j=0;j<11;j++){uint32_t a=ak[j]<0?0u-(uint32_t)ak[j]:(uint32_t)ak[j];if(a>peak)peak=a;}
 int shift=8-__builtin_clz(peak); /* largest coefficient in [2^23,2^24). */
 for(unsigned j=0;j<11;j++)q[j]=shift>=0?ak[j]>>shift:(int32_t)((uint32_t)ak[j]<<-shift);
 int32_t pr=0,pi=0;
 /* Evaluate even bins and interpolate the complex response at odd bins. */
 for(unsigned k=0;k<=256;k+=2) {
  int32_t r,i;m17_fixed_lpc_response(q,11,k,&r,&i);
  if(k<256){
   uint32_t a=r<0?0u-(uint32_t)r:(uint32_t)r,b=i<0?0u-(uint32_t)i:(uint32_t)i;
   uint32_t max=a>b?a:b;int down=max?17-__builtin_clz(max):0;if(down<0)down=0;
   a>>=down;b>>=down;
   int32_t p=log2q(a*a+b*b)+2*(down+shift-22)*65536;
   power[k]=logadd(p,-1306235); /* log2(1e-6) floor from Codec2. */
  }
  if(re){
   if(k){
    int64_t dr=(int64_t)r-pr,di=(int64_t)i-pi;
    re[k-1]=pr+(int32_t)(dr/2);
    im[k-1]=pi+(int32_t)(di/2);
   }
   re[k]=r;im[k]=i;pr=r;pi=i;
  }
 }
}
static void amplitudes(const uint32_t lsp[10],uint32_t wo,int L,int32_t energy,int32_t re[257],int32_t im[257],int32_t amp[81]) {
 int32_t ak[11],ag[11],a[256],g[256],pw[256],inv[256];
 lsp_lpc(lsp,ak);
 for(unsigned j=0;j<11;j++)ag[j]=ak[j]>>j;
 spectrum(ak,re,im,a);spectrum(ag,0,0,g);
 int32_t maxinv=INT32_MIN,maxpw=INT32_MIN;
 for(unsigned k=0;k<256;k+=2) {
  inv[k]=-a[k];pw[k]=inv[k]+(g[k]-a[k])/5;
  if(inv[k]>maxinv)maxinv=inv[k];
  if(pw[k]>maxpw)maxpw=pw[k];
 }
 uint32_t before=0,after=0;
 for(unsigned k=0;k<256;k+=2) {
  before+=2*exp2q(inv[k]-maxinv+20*65536);
  pw[k]=(int32_t)exp2q(pw[k]-maxpw+20*65536);after+=2*(uint32_t)pw[k];
 }
 for(unsigned k=0;k<254;k+=2){
  pw[k+1]=pw[k]+(pw[k+2]-pw[k])/2;
 }
 pw[255]=pw[254];
 for(unsigned k=0;k<64;k++)pw[k]=mul(pw[k],2104533975,30); /* 1.96 */
 int32_t gain=energy+log2q(before)-log2q(after)+maxinv-20*65536;
 for(int m=1;m<=L;m++) {
  unsigned am=(uint32_t)(((uint64_t)wo*(2*m-1)+8388608)>>24);
  unsigned bm=(uint32_t)(((uint64_t)wo*(2*m+1)+8388608)>>24);
  if(am>256)am=256;
  if(bm>256)bm=256;
  uint32_t sum=0;for(unsigned k=am;k<bm;k++)sum+=(uint32_t)pw[k];
  amp[m]=sum?(int32_t)exp2q((log2q(sum)+gain)/2+8*65536):0;
  if(amp[m]>33554431)amp[m]=33554431;
 }
}
static int16_t q8_to_pcm(int32_t sample) {
    const int32_t pcm = sample / 256;
    return pcm > 32767 ? 32767 : pcm < -32767 ? -32767 : (int16_t)pcm;
}
static uint32_t sample_peak(const int32_t *samples, unsigned count) {
 uint32_t peak=0;
 for(unsigned j=0;j<count;j++) {
  uint32_t magnitude=samples[j]<0?0u-(uint32_t)samples[j]:(uint32_t)samples[j];
  if(magnitude>peak)peak=magnitude;
 }
 return peak;
}
static void synthesize(fixed_decoder *s,int16_t *out,uint32_t wo,int L,int voiced,int32_t amp[81],const int32_t re[257],const int32_t im[257]) {
 int32_t r[257]={0},i[257]={0},pcm[512];
 uint32_t energy=1,maximum=1;
 for(int m=1;m<=L;m++)if((uint32_t)amp[m]>maximum)maximum=(uint32_t)amp[m];
 int eshift=19-__builtin_clz(maximum);if(eshift<0)eshift=0;
 for(int m=1;m<=L;m++){uint32_t a=(uint32_t)amp[m]>>eshift;energy+=a*a;}
 /* Store the background estimate in log2 amplitude, Q16. */
 int32_t level=(log2q(energy)-log2q((uint32_t)L))/2+(eshift-8)*65536;
 if(!voiced && level<435412)s->bg+=(level-s->bg)/10; /* 40 dB power */
 uint32_t threshold=exp2q(s->bg+65312+8*65536); /* +6 dB amplitude */
 s->phase+=wo*80u;
 for(int m=1;m<=L;m++) {
  unsigned bin=(uint32_t)(((uint64_t)wo*m+4194304)>>23);if(bin>=256)bin=255;
  uint32_t phase=voiced?s->phase*(unsigned)m:random_phase(s);
  int32_t exr=cosine(phase)>>15,exi=cosine(phase-0x40000000u)>>15;
  uint32_t ar=re[bin]<0?0u-(uint32_t)re[bin]:(uint32_t)re[bin];
  uint32_t ai=im[bin]<0?0u-(uint32_t)im[bin]:(uint32_t)im[bin];
  uint32_t peak=ar>ai?ar:ai;int down=peak?17-__builtin_clz(peak):0;if(down<0)down=0;
  int32_t hr=re[bin]>>down,hi=-(im[bin]>>down);
  uint32_t norm=root((uint32_t)(hr*hr)+(uint32_t)(hi*hi));
  int32_t pr=0,pi=0;
  if(norm){hr=hr*32767/(int32_t)norm;hi=hi*32767/(int32_t)norm;
   pr=(hr*exr-hi*exi)>>15;pi=(hi*exr+hr*exi)>>15;}
  if(voiced && (uint32_t)amp[m]<threshold){phase=random_phase(s);pr=cosine(phase)>>15;pi=cosine(phase-0x40000000u)>>15;}
  r[bin]=mul(amp[m],pr,15);i[bin]=mul(amp[m],pi,15);
 }
 m17_fixed_ifft(r,i,pcm);
 memmove(s->overlap,s->overlap+80,79*sizeof(int32_t));s->overlap[79]=0;
 for(unsigned j=0;j<79;j++)s->overlap[j]+=mul(pcm[433+j],(int32_t)j*13421773,30);
 for(unsigned j=0;j<=80;j++)s->overlap[79+j]=mul(pcm[j],(j?81-(int32_t)j:79)*13421773,30);
 uint32_t peak=sample_peak(s->overlap,80);
 if(peak>30000*256){uint32_t ratio=(30000u*256u)/(peak>>15);uint32_t gain=ratio*ratio>>15;
  for(unsigned j=0;j<80;j++)s->overlap[j]=mul(s->overlap[j],(int32_t)gain,15);
  peak=sample_peak(s->overlap,80);
 }
 peak>>=8;
 s->last_pcm_peak=peak>32767u?32767u:peak;
 for(unsigned j=0;j<80;j++)out[j]=q8_to_pcm(s->overlap[j]);
}
static void conceal_packet(fixed_decoder *s, int16_t *speech) {
 /* The tapered overlap has not passed synthesis peak control. Scale it
  * as a block to preserve the fade and stay below the last playback peak. */
 const uint32_t peak=(sample_peak(s->overlap+80,79)+255u)>>8;
 if(peak>s->last_pcm_peak) {
  const int32_t gain=(int32_t)((s->last_pcm_peak<<15)/peak);
  for(unsigned j=0;j<79;j++)speech[j]=q8_to_pcm(mul(s->overlap[80+j],gain,15));
 } else {
  for(unsigned j=0;j<79;j++)speech[j]=q8_to_pcm(s->overlap[80+j]);
 }
 memset(speech+79,0,81*sizeof *speech);
 memset(s->overlap,0,sizeof s->overlap);
 s->last_pcm_peak=0;
}
void m17_fixed_decoder_init(void *state) {
 fixed_decoder *s=state;memset(s,0,sizeof *s);s->random=1;s->wo=26843546;
 for(unsigned j=0;j<10;j++)s->lsp[j]=(uint32_t)(((uint64_t)j<<31)/11);
}
/* For an invalid LSP vector, try the 50 possible one-bit changes to
 * its Gray-coded index fields. Score the reconstructed frequencies. */
static bool repair_lsp(const fixed_decoder *s, unsigned lsp_hz[10],
                       const unsigned indices[10])
{
    if (s->lsp[0] == 0)
        return false; /* No previous accepted shape to compare. */

    int32_t previous[10];
    for (unsigned j = 0; j < 10; ++j)
        previous[j] = (int32_t)(((uint64_t)s->lsp[j] * 8000 + (1ull << 31)) >> 32);

    uint32_t best = UINT32_MAX, runner_up = UINT32_MAX;
    unsigned best_field = 0;
    int32_t correction = 0;
    for (unsigned field = 0; field < 10; ++field) {
        const unsigned index = indices[field];
        const unsigned gray = index ^ (index >> 1);
        for (unsigned bit = 1; bit < 32; bit <<= 1) {
            unsigned candidate = gray ^ bit;
            candidate ^= candidate >> 1;
            candidate ^= candidate >> 2;
            candidate ^= candidate >> 4;
            const int32_t shift = (int32_t)delta_lsp_cb[field][candidate] -
                                  (int32_t)delta_lsp_cb[field][index];
            if ((int32_t)lsp_hz[9] + shift >= 4000)
                continue;
            uint32_t cost = 0;
            for (unsigned j = 0; j < 10; ++j) {
                const int32_t delta = (int32_t)lsp_hz[j] +
                                     (j >= field ? shift : 0) - previous[j];
                if (delta < -600 || delta > 600) {
                    cost = UINT32_MAX;
                    break;
                }
                cost += (uint32_t)(delta * delta);
            }
            if (cost < best) {
                runner_up = best;
                best = cost;
                best_field = field;
                correction = shift;
            } else if (cost < runner_up) {
                runner_up = cost;
            }
        }
    }
    if (best > 10u * 250 * 250 || runner_up <= best + best / 16 + 1250u)
        return false;
    for (unsigned j = best_field; j < 10; ++j)
        lsp_hz[j] = (unsigned)((int32_t)lsp_hz[j] + correction);
    return true;
}

bool m17_fixed_decode(void *state,int16_t *speech,const uint8_t *bits) {
 fixed_decoder *s=state;unsigned at=0;int v0=unpack(bits,&at,1),v1=unpack(bits,&at,1);
 unsigned wi=(unsigned)unpack(bits,&at,7),ei=(unsigned)unpack(bits,&at,5);
 uint32_t wo=26843546u+(uint32_t)((wi*7340032u+2)/5);
 int32_t e=-217706+(int32_t)((ei*1088529u+16)/32);
 uint32_t lsp[10];
 unsigned lsp_hz[10], indices[10], hz=0;
 for(unsigned j=0;j<10;j++) {
  indices[j]=(unsigned)unpack(bits,&at,5);
  hz+=delta_lsp_cb[j][indices[j]];
  lsp_hz[j]=hz;
 }
 /* Positive gaps make the final LSP the highest. Validate it before synthesis. */
 if(hz>=4000u&&!repair_lsp(s,lsp_hz,indices)) {
  conceal_packet(s,speech);
  s->phase+=160u*s->wo;
  return false;
 }
 for(unsigned j=0;j<10;j++)lsp[j]=(uint32_t)(((uint64_t)lsp_hz[j]<<32)/8000);
 uint32_t iw=26843546;
 if(v0&&!s->voiced&&!v1)v0=0;
 if(v0)iw=s->voiced?(v1?s->wo/2+wo/2:s->wo):wo;
 for(unsigned half=0;half<2;half++) {
  uint32_t ls[10],w=half?wo:iw;int32_t re[257],im[257],amp[81];
  for(unsigned j=0;j<10;j++)ls[j]=half?lsp[j]:s->lsp[j]/2+lsp[j]/2;
  int L=(int)(0x80000000u/w);if(L>80)L=80;if(L<1)L=1;
  amplitudes(ls,w,L,half?e:(s->energy+e)/2,re,im,amp);
  synthesize(s,speech+half*80,w,L,half?v1:v0,amp,re,im);
 }
 s->wo=wo;s->voiced=v1;s->energy=e;memcpy(s->lsp,lsp,sizeof lsp);
 return v0||v1;
}

typedef struct __attribute__((may_alias)) {
 int16_t speech[320];
 int32_t square[320],filtered[64],memx,memy;
 unsigned previous_bin;
 int32_t windowed[320],input[512],re[512],im[512];
 uint32_t power[512];
} fixed_encoder;
_Static_assert(sizeof(fixed_encoder)<=sizeof(codec2_encoder_t),"encoder storage");
static int32_t clamp32(int64_t x) {return x>INT32_MAX?INT32_MAX:x<INT32_MIN?INT32_MIN:(int32_t)x;}
static int32_t shift32(int32_t x,int down) {
 return down>=0?x>>down:(int32_t)((uint32_t)x<<-down);
}
static void nlp_advance(fixed_encoder *s,const int16_t speech[80]) {
 memmove(s->speech,s->speech+80,240*sizeof *s->speech);
 memcpy(s->speech+240,speech,80*sizeof *speech);
 for(unsigned i=240;i<320;i++) {
  int32_t x=(int32_t)s->speech[i]*s->speech[i];
  int32_t y=clamp32((int64_t)x-s->memx+mul(s->memy,1020054733,30));
  s->memx=x;s->memy=y;s->square[i]=y==INT32_MAX?y:y+1;
 }
 memmove(s->filtered,s->filtered+16,48*sizeof(int32_t));
 for(unsigned i=0;i<16;i++) {
  int64_t sum=0;
  for(unsigned p=0;p<5;p++)for(unsigned tap=0;tap<10;tap++) {
   int at=240+5*(int)i-(int)p-5*(int)tap;
   sum+=mul(s->square[at],c2_nlp_taps[p*10+tap],30);
  }
  s->filtered[48+i]=clamp32(sum);
 }
 memmove(s->square,s->square+80,240*sizeof(int32_t));
}
static unsigned pitch_estimate(fixed_encoder *s) {
 int32_t *input=s->input,*re=s->re,*im=s->im;uint32_t *power=s->power;
 memset(input,0,512*sizeof *input);
 for(unsigned i=0;i<64;i++)input[i]=mul(s->filtered[i],c2_nlp_window[i],15);
 m17_fixed_fft(input,re,im);
 uint32_t peak=1;for(unsigned i=0;i<257;i++){
  uint32_t a=re[i]<0?0u-(uint32_t)re[i]:(uint32_t)re[i],b=im[i]<0?0u-(uint32_t)im[i]:(uint32_t)im[i];
  if(a>peak)peak=a;
  if(b>peak)peak=b;
 }
 int down=20-__builtin_clz(peak);if(down<0)down=0;
 for(unsigned i=0;i<257;i++){int32_t a=re[i]>>down,b=im[i]>>down;power[i]=(uint32_t)(a*a+b*b);}
 unsigned best=16;uint32_t maximum=0;
 for(unsigned i=16;i<=128;i++)if(power[i]>maximum){best=i;maximum=power[i];}
 unsigned candidate=best;
 for(unsigned multiple=2;best/multiple>=16;multiple++) {
  unsigned b=best/multiple,lo=b*4/5,hi=b*6/5;if(lo<16)lo=16;
  uint32_t threshold=(uint32_t)((uint64_t)maximum*3/((s->previous_bin>lo&&s->previous_bin<hi)?20:10));
  unsigned at=lo;uint32_t max=0;
  for(unsigned i=lo;i<=hi;i++)if(power[i]>max){max=power[i];at=i;}
  if(max>threshold&&at>0&&at<256&&max>power[at-1]&&max>power[at+1])candidate=at;
 }
 s->previous_bin=candidate;
 return 655360/candidate; /* period in Q8 samples, 512 * 5 / bin */
}
static uint32_t bin_step(unsigned period) {return (uint32_t)(((uint64_t)1<<33)/period);}
static unsigned refine_pitch(const uint32_t power[512],unsigned period) {
 for(unsigned stage=0;stage<2;stage++) {
  unsigned L=period/512; /* Each search uses its current pitch model. */
  if(L>80)L=80;
  int span=stage?256:1280,step=stage?64:256;
  uint32_t maximum=0;unsigned best=period;
  for(int p=(int)period-span;p<=(int)period+span;p+=step) {
   if(p<256)continue;
   uint32_t delta=bin_step((unsigned)p),bin=delta,total=0;
   for(unsigned m=1;m<=L;m++,bin+=delta){unsigned b=(bin+32768)>>16;if(b<512)total+=power[b];}
   if(total>maximum){maximum=total;best=(unsigned)p;}
  }
  period=best;
 }
 if(period<20*256)period=20*256;
 if(period>160*256)period=160*256;
 return period;
}
static int voicing(const int32_t re[512],const int32_t im[512],const uint32_t power[512],unsigned period) {
 unsigned step=bin_step(period),L=period/512;
 if(L>80)L=80;
 /* Same 0.95*pi guard used by the Codec2 analysis model. */
 if(L*51200>=period*95)L--;
 uint32_t band[81]={0};uint64_t low=1,high=1,signal=1,error=1;
 for(unsigned m=1;m<=L;m++) {
  unsigned am=(unsigned)(((uint64_t)(2*m-1)*step+65536)>>17),bm=(unsigned)(((uint64_t)(2*m+1)*step+65536)>>17);
  if(bm>256)bm=256;
  for(unsigned k=am;k<bm;k++)band[m]+=power[k];
  if(m<=L/2){low+=band[m];if(m<=L/4)signal+=band[m];}
  if(m>=L/2)high+=band[m];
 }
 for(unsigned m=1;m<=L/4;m++) {
  unsigned al=(unsigned)(((uint64_t)(2*m-1)*step+131071)>>17),bl=(unsigned)(((uint64_t)(2*m+1)*step+131071)>>17);
  int offset=(int)((256u<<16)-m*step+32768)>>16;
  if(offset< -(int)al)offset=-(int)al;
  if(offset+(int)bl>=512)offset=511-(int)bl;
  if(bl>512)bl=512;
  int64_t nr=0,ni=0;uint32_t den=0;
  for(unsigned k=al;k<bl;k++){int32_t w=c2_window_spectrum[offset+(int)k];nr+=re[k]*w;ni+=im[k]*w;den+=(uint32_t)(w*w);}
  if(!den)continue;
  int32_t ar=(int32_t)(nr*32768/den),ai=(int32_t)(ni*32768/den);
  for(unsigned k=al;k<bl;k++) {
   int32_t w=c2_window_spectrum[offset+(int)k];
   int32_t dr=re[k]-mul(ar,w,15),di=im[k]-mul(ai,w,15);
   error+=(uint64_t)((int64_t)dr*dr)+(uint64_t)((int64_t)di*di);
  }
 }
 int voiced=signal*10000>error*39811; /* 6 dB */
 if(!voiced&&low>high*10)voiced=1;
 if(voiced&&(low*10<high||(period>=34133&&low*10000<high*3981)))voiced=0;
 return voiced;
}
static int32_t chebyshev(const int32_t q[6],int32_t x) {
 int32_t b1=0,b2=0;
 for(unsigned j=0;j<5;j++){int32_t b=(int32_t)((uint32_t)q[j]+m17_smul_shift(x,b1,29)-(uint32_t)b2);b2=b1;b1=b;}
 return (int32_t)((uint32_t)q[5]+m17_smul_shift(x,b1,30)-(uint32_t)b2);
}
static uint32_t acos_turn(int32_t x) {
 unsigned lo=0,hi=2048;
 while(lo+1<hi){unsigned m=(lo+hi)/2;if(c2_cos[m]>=x)lo=m;else hi=m;}
 uint32_t d=(uint32_t)(c2_cos[lo]-c2_cos[hi]);
 uint32_t f=d?(uint32_t)(((uint64_t)(c2_cos[lo]-x)<<20)/d):0;
 if(f>1048576)f=1048576;
 return (lo<<20)+f;
}
static unsigned lpc_lsps(const int32_t ak[11],uint32_t lsps[10]) {
 int32_t p[6],q[6];p[0]=q[0]=1<<22;
 for(unsigned i=1;i<=5;i++){p[i]=ak[i]+ak[11-i]-p[i-1];q[i]=ak[i]-ak[11-i]+q[i-1];}
 for(unsigned i=0;i<5;i++){p[i]*=2;q[i]*=2;}
 for(unsigned which=0;which<2;which++) {
  int32_t *a=which?q:p;uint32_t peak=1;
  for(unsigned i=0;i<6;i++){uint32_t v=a[i]<0?0u-(uint32_t)a[i]:(uint32_t)a[i];if(v>peak)peak=v;}
  int down=7-__builtin_clz(peak);
  for(unsigned i=0;i<6;i++)a[i]=shift32(a[i],down);
 }
 int32_t xl=1<<30;unsigned roots=0;
 for(unsigned j=0;j<10;j++) {
  const int32_t *a=(j&1)?q:p;int32_t left=chebyshev(a,xl),xr=xl;
  while(xr>=-(1<<30)) {
   xr=xl-10737418;int32_t right=chebyshev(a,xr);
   if((right<=0&&left>=0)||(right>=0&&left<=0)) {
    int32_t xm=0;
    for(unsigned n=0;n<6;n++){xm=xl/2+xr/2;int32_t mid=chebyshev(a,xm);
     if((mid^left)>=0&&mid&&left){xl=xm;left=mid;}else xr=xm;}
    lsps[j]=acos_turn(xm);xl=xm;roots++;break;
   }
   left=right;xl=xr;
  }
 }
 return roots;
}
static unsigned lpc_analyze(const int32_t windowed[320],uint32_t lsps[10]) {
 uint32_t peak=1;
 for(unsigned i=21;i<300;i++){uint32_t a=windowed[i]<0?0u-(uint32_t)windowed[i]:(uint32_t)windowed[i];if(a>peak)peak=a;}
 int down=18-__builtin_clz(peak);int16_t samples[279];
 for(unsigned i=0;i<279;i++)samples[i]=(int16_t)shift32(windowed[21+i],down);
 int32_t r[11],a[11]={1<<22},prev[11];
 for(unsigned lag=0;lag<=10;lag++){int64_t sum=0;for(unsigned i=0;i<279-lag;i++)sum+=(int32_t)samples[i]*samples[i+lag];r[lag]=(int32_t)(sum>>6);}
 int rs=r[0]>0?9-__builtin_clz((uint32_t)r[0]):0;if(rs<0)rs=0;
 for(unsigned i=0;i<=10;i++)r[i]>>=rs;
 int32_t energy=r[0];
 for(unsigned order=1;order<=10&&energy>0;order++) {
  memcpy(prev,a,sizeof a);int64_t sum=r[order];
  for(unsigned j=1;j<order;j++)sum+=mul(prev[j],r[order-j],22);
  int32_t k=0;
  if(sum<=energy&&sum>=-energy)k=(int32_t)(-sum*(1<<30)/energy);
  a[order]=k>>8;
  for(unsigned j=1;j<order;j++)a[j]=prev[j]+mul(k,prev[order-j],30);
  energy=mul(energy,(1<<30)-mul(k,k,30),30);
 }
 int64_t residual=0;for(unsigned j=0;j<=10;j++)residual+=mul(a[j],r[j],22);
 if(residual<1)residual=1;
 int32_t elog=log2q((uint32_t)residual)+(2*down-10+rs)*65536;
 for(unsigned j=0;j<=10;j++)a[j]=mul(a[j],c2_bw_gamma[j],30);
 if(lpc_lsps(a,lsps)!=10)for(unsigned j=0;j<10;j++)lsps[j]=(uint32_t)(((uint64_t)j<<31)/10);
 int index=((elog+217706)*32+544264)/1088529;
 return index<0?0:index>31?31:(unsigned)index;
}
void m17_fixed_encoder_init(void *state) {
 fixed_encoder *s=state;memset(s,0,sizeof *s);for(unsigned i=0;i<320;i++)s->speech[i]=1;s->previous_bin=16;
}
#include "octave_check.h"

static unsigned analyze_voicing(fixed_encoder *s,int *voiced)
{
 int32_t *windowed=s->windowed,*input=s->input,*re=s->re,*im=s->im;
 uint32_t *power=s->power;
 unsigned initial=pitch_estimate(s);memset(input,0,512*sizeof *input);
 for(unsigned j=21;j<300;j++)windowed[j]=((int32_t)s->speech[j]*c2_window[j])>>15;
 for(unsigned j=0;j<139;j++){input[j]=windowed[j+160];input[373+j]=windowed[j+21];}
 m17_fixed_fft(input,re,im);
 uint32_t peak=1;for(unsigned j=0;j<257;j++){
  uint32_t a=re[j]<0?0u-(uint32_t)re[j]:(uint32_t)re[j],b=im[j]<0?0u-(uint32_t)im[j]:(uint32_t)im[j];if(a>peak)peak=a;if(b>peak)peak=b;}
 int shift=20-__builtin_clz(peak);if(shift<0)shift=0;
 for(unsigned j=0;j<257;j++){re[j]>>=shift;im[j]>>=shift;power[j]=(uint32_t)(re[j]*re[j]+im[j]*im[j]);}
 for(unsigned j=1;j<256;j++){re[512-j]=re[j];im[512-j]=-im[j];power[512-j]=power[j];}
 unsigned period=refine_pitch(power,initial);
 *voiced=voicing(re,im,power,period);
 if (*voiced && initial <= SAMP_RATE * 256 / 160 &&
     period <= SAMP_RATE * 256 / 145) {
  /* The FFT has consumed input. Reuse it for the octave check. */
  const unsigned lower = octave_lower_period(s->speech, initial, input);
  if (lower) {
   const unsigned candidate = refine_pitch(power, lower);
   if (voicing(re, im, power, candidate))
    period = candidate;
  }
 }
 return period;
}

void m17_fixed_encode(void *state,uint8_t *bits,const int16_t *speech) {
 fixed_encoder *s=state;
 int first_voiced,voiced;
 uint32_t lsps[10];
 nlp_advance(s,speech);
 (void)analyze_voicing(s,&first_voiced);
 nlp_advance(s,speech+80);
 unsigned period=analyze_voicing(s,&voiced);
 uint32_t wo=(uint32_t)(((uint64_t)1<<40)/period);
 int wi=((int32_t)wo-26843546+734003)/1468006;if(wi<0)wi=0;if(wi>127)wi=127;
 unsigned ei=lpc_analyze(s->windowed,lsps),at=0;memset(bits,0,8);
 pack(bits,&at,(unsigned)first_voiced,1);pack(bits,&at,(unsigned)voiced,1);pack(bits,&at,(unsigned)wi,7);pack(bits,&at,ei,5);
 unsigned last=0;
 for(unsigned j=0;j<10;j++){
  int32_t target=(int32_t)(((uint64_t)lsps[j]*8000)>>16)-(int32_t)(last<<16);
  const uint16_t *cb=delta_lsp_cb[j];unsigned best=0;int64_t delta=INT64_MAX;
  for(unsigned k=0;k<32;k++){int64_t d=(int32_t)(cb[k]<<16)-target;if(d<0)d=-d;if(d<delta){delta=d;best=k;}}
  last+=cb[best];pack(bits,&at,best,5);
 }
}

void codec2_encoder_init(codec2_encoder_t *state)
{
 m17_fixed_encoder_init(state);
}

void codec2_decoder_init(codec2_decoder_t *state)
{
 m17_fixed_decoder_init(state);
}

void codec2_encode(codec2_encoder_t *state, uint8_t *bits, const int16_t *speech)
{
 m17_fixed_encode(state,bits,speech);
}

void codec2_decode(codec2_decoder_t *state, int16_t *speech, const uint8_t *bits)
{
 m17_fixed_decode(state,speech,bits);
}
