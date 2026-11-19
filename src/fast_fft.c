/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Codec2 integer 512-point FFT, with Q15 twiddles and a /2 scale each stage.
 * Scratch is stack-owned, so independent codec states remain reentrant. */
#include "fixed_codec.h"
#include "fft_tables.h"
#include "wide_math.h"
static void fft(int32_t r[256],int32_t im[256],int inverse) {
 for(unsigned i=0;i<256;i++) {unsigned j=fft_reverse[i]>>1;if(j>i){int32_t v=r[i];r[i]=r[j];r[j]=v;v=im[i];im[i]=im[j];im[j]=v;}}
 for(unsigned n=2;n<=256;n*=2)for(unsigned at=0;at<256;at+=n)for(unsigned j=0;j<n/2;j++) {
  unsigned k=j*512/n,a=at+j,b=a+n/2;
  int32_t c=fft_cos[k],s=inverse?-fft_sin[k]:fft_sin[k];
  int32_t tr=(int32_t)((r[b]*c-im[b]*s)>>15);
  int32_t ti=(int32_t)((r[b]*s+im[b]*c)>>15);
  int32_t ar=r[a],ai=im[a];r[a]=(ar+tr+1)>>1;im[a]=(ai+ti+1)>>1;r[b]=(ar-tr+1)>>1;im[b]=(ai-ti+1)>>1;
 }
}
/* Clenshaw evaluation of a degree <= 15 LPC polynomial. Q24 coefficients
 * leave room for the recurrence's worst-case 120-fold growth. This uses
 * one wide multiply per coefficient instead of separate sine/cosine sums. */
void m17_fixed_lpc_response(const int32_t *q,unsigned n,unsigned k,int32_t *re,int32_t *im) {
 int32_t b1=0,b2=0,c=lpc_cos[k];
 for(unsigned j=n;j-->1;) {
  int32_t b=(int32_t)((uint32_t)q[j]+m17_smul_shift(c,b1,29)-(uint32_t)b2);
  b2=b1;b1=b;
 }
 *re=(int32_t)((uint32_t)q[0]+m17_smul_shift(c,b1,30)-(uint32_t)b2);
 *im=(int32_t)m17_smul_shift(lpc_sin[k],b1,30);
}
static int32_t scale_integer(int32_t x,int shift) {
 if(shift>=0)return x>>shift;
 int64_t v=(int64_t)x*(1u<<-shift);
 return v>INT32_MAX?INT32_MAX:v<INT32_MIN?INT32_MIN:(int32_t)v;
}
void m17_fixed_ifft(const int32_t *re,const int32_t *imag,int32_t *out) {
 uint32_t peak=1;int32_t r[256],im[256];
 for(unsigned i=0;i<257;i++) {
  uint32_t a=re[i]<0?0u-(uint32_t)re[i]:(uint32_t)re[i];
  uint32_t b=imag[i]<0?0u-(uint32_t)imag[i]:(uint32_t)imag[i];
  if(a>peak)peak=a;if(b>peak)peak=b;
 }
 int shift=17-__builtin_clz(peak);
 for(unsigned k=0;k<256;k++) {
  unsigned b=256-k;
  int32_t ar=scale_integer(re[k],shift),ai=scale_integer(imag[k],shift);
  int32_t br=scale_integer(re[b],shift),bi=scale_integer(imag[b],shift);
  int32_t er=(ar+br)/2,ei=(ai-bi)/2,dr=(ar-br)/2,di=(ai+bi)/2;
  int32_t c=fft_cos[k],s=fft_sin[k];
  int32_t ore=(dr*c+di*s)>>15,oi=(di*c-dr*s)>>15;
  r[k]=er-oi;im[k]=ei+ore;
 }
 fft(r,im,1);
 for(unsigned i=0;i<256;i++) {
  out[2*i]=scale_integer(r[i]*512,-shift);
  out[2*i+1]=scale_integer(im[i]*512,-shift);
 }
}
/* Returns a block exponent: spectrum = result * 2^(return value).
 * Normalize only once at the boundary; all butterflies stay integer. */
int m17_fixed_fft(const int32_t *in,int32_t *re,int32_t *imag) {
 uint32_t peak=1;int32_t r[256],im[256];
 for(unsigned j=0;j<512;j++){uint32_t a=in[j]<0?0u-(uint32_t)in[j]:(uint32_t)in[j];if(a>peak)peak=a;}
 int shift=17-__builtin_clz(peak);
 for(unsigned j=0;j<256;j++){r[j]=scale_integer(in[2*j],shift);im[j]=scale_integer(in[2*j+1],shift);}
 fft(r,im,0);
 for(unsigned k=0;k<=256;k++) {
  unsigned a=k&255,b=(256-k)&255;
  int32_t er=(r[a]+r[b])/2,ei=(im[a]-im[b])/2;
  int32_t ore=(im[a]+im[b])/2,oi=(r[b]-r[a])/2;
  int32_t c=k==256?-32768:fft_cos[k],s=k==256?0:fft_sin[k];
  re[k]=er+((ore*c-oi*s)>>15);imag[k]=ei+((ore*s+oi*c)>>15);
 }
 return shift+8;
}
