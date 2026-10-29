#!/usr/bin/env python3
"""Real persistent-range observer regression; no hardware claims."""
import argparse,subprocess,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
p=argparse.ArgumentParser();p.add_argument('--cxx',default='c++');p.add_argument('--sanitize',action='store_true');a=p.parse_args()
with tempfile.TemporaryDirectory() as d:
    flags=['-std=c++20','-O1' if a.sanitize else '-O2','-g','-Wall','-Wextra','-Werror','-pedantic','-pthread']
    if a.sanitize: flags+=['-fsanitize=address,undefined','-fno-omit-frame-pointer']
    out=Path(d)/'test'
    subprocess.run([a.cxx,*flags,'-I',str(ROOT/'vita3k/util/include'),str(ROOT/'vita3k/util/tests/switch_write_watch_v4.cpp'),'-o',str(out)],check=True)
    subprocess.run([str(out)],check=True,timeout=45)
print('v4 real observer OK: exact/persistent/alias/palette/cross-page, nonsticky CPU and host evidence, cache-ledger gaps, remap tokens, bounds')
