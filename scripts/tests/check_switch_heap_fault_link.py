#!/usr/bin/env python3
"""Verify the FINAL A64 ELF routes crt0 exceptions through the scoped wrapper.

Symbol presence alone is insufficient: inspect the actual crt0 branch as well.
This checks linkage, not that the device kernel successfully retries a store.
"""
import argparse
from pathlib import Path
import re
import struct
import subprocess

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('elf',type=Path)
p.add_argument('--objdump',default='aarch64-none-elf-objdump')
p.add_argument('--nm',default='aarch64-none-elf-nm')
a=p.parse_args()
with a.elf.open('rb') as f: header=f.read(64)
assert header[:6]==b'\x7fELF\x02\x01' and struct.unpack_from('<H',header,18)[0]==183, 'Expected little-endian AArch64 ELF64'
entry=struct.unpack_from('<Q',header,24)[0]
symbols=subprocess.check_output([a.nm,'--defined-only',str(a.elf)],text=True)
for symbol in ('__wrap___libnx_exception_entry','__libnx_exception_entry','v3k_try_heap_fault','v3k_heap_register_test'):
 assert re.search(r'\b'+re.escape(symbol)+r'$',symbols,re.M), 'Missing '+symbol
text=subprocess.check_output([a.objdump,'-d','--start-address='+hex(entry),'--stop-address='+hex(entry+512),str(a.elf)],text=True)
assert re.search(r'\bb\s+[^\n]*<__wrap___libnx_exception_entry>',text), 'crt0 is not wrapped; refuse a deliberate fault build'
print('Final A64 ELF: actual crt0 branch -> scoped wrapper; stock fallback and native register test linked')
