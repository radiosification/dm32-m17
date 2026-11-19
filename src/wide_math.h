/* SPDX-License-Identifier: GPL-3.0-or-later */
/* High product bits using only 32-bit multiplies. CK803S's generic 64-bit
 * runtime also computes cross terms for two full 64-bit inputs. */
#pragma once
#include <stdint.h>
static inline uint32_t m17_product_hi(uint32_t a,uint32_t b,uint32_t *middle) {
 uint32_t al=a&65535,ah=a>>16,bl=b&65535,bh=b>>16;
 uint32_t lo=al*bl,t=ah*bl+(lo>>16);
 uint32_t m=(t&65535)+al*bh;
 *middle=m;
 return ah*bh+(t>>16)+(m>>16);
}
static inline uint32_t m17_umul_shift30(uint32_t a,uint32_t b) {
 uint32_t m,h=m17_product_hi(a,b,&m);return (h<<2)|((m>>14)&3);
}
static inline uint32_t m17_smul_shift(int32_t a,int32_t b,unsigned shift) {
 uint32_t m,h=m17_product_hi((uint32_t)a,(uint32_t)b,&m);
 if(a<0)h-=(uint32_t)b;
 if(b<0)h-=(uint32_t)a;
 if(shift<16)return (h<<(32-shift))|(((uint32_t)a*(uint32_t)b)>>shift);
 return (h<<(32-shift))|((m>>(shift-16))&((1u<<(32-shift))-1));
}
