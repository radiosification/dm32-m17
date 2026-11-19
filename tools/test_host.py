#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
from build import *
from filter_taps import generate
LIB=[M17/'m17.c',*sorted(p for p in M17.glob('*/*.c') if p.parent.name!='unit_tests')]
def main():
 generate()
 flags=['-std=gnu11','-O2','-g','-DM17_MODEM_TEST','-I'+str(M17),'-I'+str(ROOT/'src'),'-I'+str(OUT),'-fsanitize=address,undefined','-fno-sanitize-recover=all']
 run(['clang',*flags,ROOT/'tests/test_link.c',ROOT/'src/m17_link.c',ROOT/'src/m17_modem.c',*LIB,'-lm','-o',OUT/'test-link'])
 run([OUT/'test-link'])
 numeric_flags=['-O2','-g','-fsanitize=address,undefined','-fno-sanitize-recover=all','-I'+str(ROOT/'src'),'-I'+str(C2/'inc')]
 run(['clang',*numeric_flags,'tests/test_fixed_codec.c',*codec_sources(),'-o',OUT/'test-fixed-codec'])
 run([OUT/'test-fixed-codec'])
if __name__=='__main__':main()
