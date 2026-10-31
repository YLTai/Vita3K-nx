#!/usr/bin/env python3
"""Compile actual memo + controls. Host hash is FNV, device uses actual XXH3.
Exhaustive byte mutations and seeded state-machine comparisons; not GPU proof.
"""
import argparse
import ctypes.util
from pathlib import Path
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[2]
SOURCE = r'''
#include <util/content_hash_controls.h>
#include <cassert>
#include <random>
#include <vector>
#ifdef USE_PINNED_XXHASH
#define XXH_INLINE_ALL
#include <xxhash.h>
#elif defined(USE_SYSTEM_XXHASH)
extern "C" uint64_t XXH3_64bits(const void *, size_t);
#endif
using namespace switch_validation::content;
uint64_t hash(const void *p, size_t n) {
#if defined(USE_PINNED_XXHASH) || defined(USE_SYSTEM_XXHASH)
    return XXH3_64bits(p, n);
#else
    uint64_t h=1469598103934665603ULL;
    for (size_t i=0;i<n;++i) {h^=static_cast<const uint8_t*>(p)[i];h*=1099511628211ULL;}
    return h;
#endif
}
int main() {
    unsigned passed=0;
    run_controls(hash,[&](const char*,bool ok){assert(ok);++passed;});
    assert(passed==control_names.size());
    std::mt19937 rng(0x51a7);
    std::array<std::array<uint8_t,8192>,8> data{};
    Memo m(32768,5,16);Counts c;
    for(unsigned n=0;n<25000;++n){
        auto &v=data[rng()%8]; auto &pal=data[rng()%8];
        if(n%3) v[rng()%v.size()]^=uint8_t(rng());
        const size_t sz=1+rng()%v.size(), ps=n%4?0:32;
        Input input{v.data(),sz,pal.data(),ps};
        const auto mode=n%7==0?Mode::Audit:Mode::Compare;
        assert(m.evaluate(input,mode,hash,c)==full_digest(input,hash));
        assert(m.resident_bytes()<=32768 && m.peak_bytes()<=32768);
        if(n%101==0)m.clear();
    }
    // Every byte, including a non-multiple-of-word tail and separate palette.
    std::vector<uint8_t> px(1025),pal(65);Memo e(4096,8,1);Counts ec;
    Input i{px.data(),px.size(),pal.data(),pal.size()};
    e.evaluate(i,Mode::Compare,hash,ec);
    for(auto *v:{&px,&pal})for(size_t k=0;k<v->size();++k){
        (*v)[k]^=0x79;assert(e.evaluate(i,Mode::Compare,hash,ec)==full_digest(i,hash));
        assert(e.evaluate(i,Mode::Compare,hash,ec)==full_digest(i,hash));
    }
    assert(ec.refreshes==1+px.size()+pal.size());
    assert(ec.hits==px.size()+pal.size());
    assert(!c.audit_disagreements && !ec.audit_disagreements);
}
'''
def main():
    p=argparse.ArgumentParser();p.add_argument('--cxx',default='c++');p.add_argument('--sanitize',action='store_true');a=p.parse_args()
    backend, extra = 'host FNV fallback', []
    if (ROOT/'external/xxHash/xxhash.h').is_file():
        backend, extra = 'pinned XXH3', ['-DUSE_PINNED_XXHASH','-I',str(ROOT/'external/xxHash')]
    elif ctypes.util.find_library('xxhash'):
        backend, extra = 'host system XXH3 (not the pinned submodule)', ['-DUSE_SYSTEM_XXHASH','-lxxhash']
    with tempfile.TemporaryDirectory() as d:
        d=Path(d);(d/'main.cpp').write_text(SOURCE)
        flags=['-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer'] if a.sanitize else ['-O2']
        subprocess.run([a.cxx,'-std=c++20','-Wall','-Wextra','-Werror','-pedantic',*flags,'-I',str(ROOT/'vita3k/util/include'),str(d/'main.cpp'),*extra,'-o',str(d/'test')],check=True)
        subprocess.run([str(d/'test')],check=True,timeout=45)
    print(f'Content memo: 12 controls, 25000 seeded transitions, all-byte/palette mutations passed with {backend}; device execution pending')
if __name__=='__main__':main()
