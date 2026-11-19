#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generate integer polyphase filters from libm17's published RRC coefficients."""
from pathlib import Path
import math,re
ROOT=Path(__file__).resolve().parents[1]
def generate():
 s=(ROOT/'vendor/libm17/math/rrc.c').read_text().split('const float rrc_taps_5[8*5+1]=')[1].split('};')[0]
 rrc=[float(v) for v in re.findall(r'[-+]?\d+\.\d+',s)]
 assert len(rrc)==41
 # 8->24 kHz interpolation: Blackman-windowed sinc, cutoff 3.8 kHz.
 interp=[]
 for i in range(121):
  x=i-60;fc=3800/24000
  v=2*fc if x==0 else math.sin(2*math.pi*fc*x)/(math.pi*x)
  interp.append(v*(.42-.5*math.cos(2*math.pi*i/120)+.08*math.cos(4*math.pi*i/120)))
 interp=[v*3/sum(interp) for v in interp]
 rx=[sum(interp[j]*rrc[i-j] for j in range(len(interp)) if 0<=i-j<len(rrc))/math.sqrt(5) for i in range(161)]
 tx=[v*math.sqrt(5) for v in rrc]
 header='/* Generated from libm17 RRC taps, GPL-2.0-or-later. */\n'
 for name,v in [('tx_taps',tx),('rx_taps',rx)]:
  header+='static const int16_t '+name+'[] = {'+','.join(str(round(x*16384)) for x in v)+'};\n'
 taps=[round(x*16384) for x in rx]
 bound=max(sum(abs(x) for x in taps[p::3]) for p in range(3))
 assert bound*32768<2**31
 header+='enum { M17_RX_TAPS_L1_BOUND = '+str(bound)+' };\n'
 header+='static const int16_t rx_polyphase[3][54] = {\n'+',\n'.join('{'+','.join(map(str,taps[p::3]+[0]*(54-len(taps[p::3]))))+'}' for p in range(3))+'\n};\n'
 (ROOT/'build/filter_taps.h').write_text(header)
if __name__=='__main__':generate()
