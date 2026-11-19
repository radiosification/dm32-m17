#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
from pathlib import Path
import subprocess
import os
import shutil
ROOT=Path(__file__).resolve().parents[1]
_toolchain_bin = os.environ.get('CSKY_BIN')
CC = (Path(_toolchain_bin).expanduser().absolute() / 'csky-elf-gcc'
      if _toolchain_bin else Path(shutil.which('csky-elf-gcc') or 'csky-elf-gcc'))
OUT=ROOT/'build';OUT.mkdir(exist_ok=True)
C2=ROOT/'vendor/Codec2-mod';M17=ROOT/'vendor/libm17'
FLAGS=['-mcpu=ck803s','-mlittle-endian','-mno-high-registers','-msoft-float','-O3','-ffast-math','-I'+str(OUT),'-ffunction-sections','-fdata-sections','-fstack-usage','-I'+str(C2/'inc'),'-I'+str(M17),'-I'+str(ROOT/'src'),'-I'+str(OUT)]
def run(cmd):subprocess.run([str(x) for x in cmd],check=True,cwd=ROOT)
def check_integer_codec(obj):
 symbols=subprocess.check_output([str(CC.with_name('csky-elf-nm')),'-u',str(obj)],text=True).split()
 bad=[s for s in symbols if (s.startswith('__') and ('sf' in s or 'df' in s)) or s in ('sinf','cosf','sqrtf','powf','logf','log2f','log10f')]
 assert not bad, 'Integer Codec2 unexpectedly calls floating-point functions: '+str(bad)
def codec_sources():
 # Keep exactly the upstream Gray-code bit packing and unpacking routines.
 # The rest of quantise.c implements the floating-point quantizer.
 quantise = (C2 / 'src/quantise.c').read_text()
 start = quantise.index('static inline unsigned int binary_to_gray(')
 end = quantise.index('int encode_Wo(', start)
 bits = OUT / 'codec2_bits.c'
 bits.write_text('#include "quantise.h"\n\n' + quantise[start:end])

 from fixed_tables import generate
 generate()
 return [ROOT / 'src/fixed_codec.c', ROOT / 'src/fast_fft.c',
         bits, C2 / 'src/delta_lsp_cb.c']
