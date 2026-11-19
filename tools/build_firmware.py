#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
from pathlib import Path
import subprocess,shutil
from build import ROOT,OUT,CC,C2,M17,FLAGS,run,check_integer_codec,codec_sources
from filter_taps import generate as filters
FW=ROOT/'firmware';OUT.mkdir(exist_ok=True)
def install():
 dst=FW/'core/m17';dst.mkdir(exist_ok=True);inc=dst/'include';inc.mkdir(exist_ok=True)
 for p in (C2/'inc').glob('*.h'):shutil.copyfile(p,inc/p.name)
 shutil.copyfile(M17/'m17.h',inc/'m17.h')
 filters();shutil.copyfile(OUT/'filter_taps.h',inc/'filter_taps.h')
 for name in ['m17_link.c','m17_link.h','m17_modem.c','m17_voice.c','m17_voice.h','mic_audio.c','mic_audio.h']:
  s=(ROOT/'src'/name).read_text()
  if name=='m17_voice.c':s=s.replace('m17_voice *v=ctx;int16_t pcm[160];(void)fn;', 'm17_voice *v=ctx;int16_t pcm[160];(void)fn;\n extern bool radio_m17_admit(const lsf_t *);if(!radio_m17_admit(&v->receiver.lsf))return;')
  (dst/name).write_text(s)
 for name in ['radio_m17.c','radio_m17.h']:shutil.copyfile(ROOT/'integration'/name,FW/'core/dv'/name)
 codec=codec_sources()
 lib=[M17/'m17.c',*[p for p in M17.glob('*/*.c') if p.parent.name!='unit_tests']]
 objects=[]
 for i,p in enumerate([*codec,*lib]):
  obj=OUT/f'firmware-codec-{i}.o';run([CC,*FLAGS,'-I'+str(inc),'-c',p,'-o',obj]);objects.append(obj)
  if i<len(codec):check_integer_codec(obj)
 target=dst/'libm17_codec.a'
 if target.exists():target.unlink()
 run([CC.with_name('csky-elf-ar'),'rcs',target,*objects])
if __name__=='__main__':
 import argparse
 parser=argparse.ArgumentParser();parser.add_argument('--stack-check',action='store_true');a=parser.parse_args()
 if not shutil.which(str(CC)):
  parser.error('Install csky-elf-gcc on PATH or set CSKY_BIN to its bin directory.')
 if not (C2/'inc').is_dir() or not (M17/'m17.h').is_file():
  parser.error('Run make bootstrap to fetch Codec2-mod and libm17.')
 install()
 args=['make','-j8','BOARD=dm32','BUILD=build/dm32-m17','CROSS='+str(CC.with_name('csky-elf-')),
 'CONFIG_M17=y']
 args+=['all']
 if a.stack_check:args+=['stack-check']
 subprocess.run(args,cwd=FW,check=True)
