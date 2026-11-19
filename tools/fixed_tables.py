# SPDX-License-Identifier: GPL-3.0-or-later
"""Generate integer Codec2 constants from the pinned codec and mathematical definitions."""
from pathlib import Path
import math,re
ROOT=Path(__file__).resolve().parents[1]
def generate():
    out=['/* SPDX-License-Identifier: GPL-3.0-or-later */','#pragma once','#include <stdint.h>']
    def table(name,ctype,values):
        out.append('static const '+ctype+' '+name+'[]={'+','.join(str(v) for v in values)+'};')
    table('c2_cos','int32_t',[round(math.cos(2*math.pi*i/4096)*(1<<30)) for i in range(4097)])
    table('c2_log','int32_t',[round(math.log2(1+i/256)*65536) for i in range(257)])
    table('c2_exp','uint32_t',[round(2**(i/256)*(1<<24)) for i in range(257)])
    table('c2_logadd','int32_t',[round(math.log2(1+2**(-i/32))*65536) for i in range(513)])
    hann=[0.5-0.5*math.cos(2*math.pi*j/278) for j in range(279)]
    norm=1/math.sqrt(sum(x*x for x in hann)*512)
    w=[0.]*320;w[21:300]=[x*norm for x in hann]
    table('c2_window','int32_t',[round(x*(1<<23)) for x in w])
    shifted=[0.]*512;shifted[:139]=w[160:299];shifted[-139:]=w[21:160]
    response=[sum(shifted[j]*math.cos(2*math.pi*k*j/512) for j in range(512)) for k in range(257)]
    wr=[response[256-i] for i in range(256)]+response[:256]
    table('c2_window_spectrum','int16_t',[round(x*32768) for x in wr])
    table('c2_nlp_window','int32_t',[round((0.5-0.5*math.cos(2*math.pi*j/63))*32768) for j in range(64)])
    text=(ROOT/'vendor/Codec2-mod/src/nlp.c').read_text()
    taps=re.findall(r'[-+]?[0-9]+\.[0-9]+',text.split('const float nlp_fir_poly')[1].split('};')[0])
    assert len(taps)==50
    table('c2_nlp_taps','int32_t',[round(float(x)*(1<<30)) for x in taps])
    table('c2_bw_gamma','int32_t',[round(.994**j*(1<<30)) for j in range(11)])
    (ROOT/'src/fixed_tables.h').write_text('\n'.join(out)+'\n')
if __name__=='__main__':generate()
