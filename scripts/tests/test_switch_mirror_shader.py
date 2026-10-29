#!/usr/bin/env python3
"""CPU-only tests for shared BDA ABI/ranges and guard/copy negative controls.
The device kernel is separately compiled by glslang and validated by spirv-val.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[2]
CPP = r'''
#include <util/mirror_shader_contract.h>
#include <array>
#include <cassert>
#include <cstring>
#include <random>
#include <algorithm>
using namespace switch_validation::mirror_shader;
using Data=std::array<uint8_t,physical>;
// Explicit CPU shader adapter, not SPIR-V execution.
void dispatch(const Data &src,Data &dst,Range r,bool copy,bool bounds=true) {
    assert(valid_range(r.offset,r.bytes));
    const uint32_t count=r.bytes/4;
    for(uint32_t i=0;i<groups(count)*64;++i) {
        if(bounds&&i>=count)continue;
        const auto address=guard+r.offset+4*i;
        assert(address+4<=physical); // all deliberate overdispatch stays allocated
        uint32_t value=0xdeadbeef;
        if(copy)std::memcpy(&value,src.data()+address,4);
        std::memcpy(dst.data()+address,&value,4);
    }
}
bool run(bool drop_first=false,bool drop_tail=false,bool drop_return=false,bool bounds=true) {
    Data gpu,scratch;gpu.fill(sentinel);scratch.fill(sentinel);
    for(size_t i=0;i<payload;++i)gpu[guard+i]=uint8_t(i*17+3);
    const auto expected=gpu;
    for(const auto r:copies) {
        if(r.bytes)std::memset(scratch.data()+guard+r.offset,0x3c,r.bytes);
        if((drop_first&&r.offset==0)||(drop_tail&&r.bytes==4))continue;
        dispatch(gpu,scratch,r,r.bytes!=0,bounds);
    }
    if(!guards_intact(scratch))return false;
    std::memset(gpu.data()+guard,0x7e,payload);
    if(!drop_return)dispatch(scratch,gpu,{0,payload},true,bounds);
    return guards_intact(gpu)&&gpu==expected;
}
int main() {
    static_assert(sizeof(Push)==32&&offsetof(Push,destination)==8&&offsetof(Push,reserved)==28);
    assert(run());assert(!run(true));assert(!run(false,true));assert(!run(false,false,true));
    assert(!run(false,false,false,false)); // missing shader bounds must NOT pass
    assert(valid_range(payload,0)&&!valid_range(payload,4));
    assert(!valid_range(SIZE_MAX,4)&&!valid_range(0,SIZE_MAX));
    assert(!valid_range(1,4)&&!valid_range(0,3));
    Data a;a.fill(sentinel);size_t bad=0;
    for(size_t i=0;i<physical;++i){a[i]^=1;
        const bool in_guard=i<guard||i>=guard+payload;
        assert(guards_intact(a,&bad)!=in_guard);if(in_guard)assert(bad==i);
        a[i]^=1;
    }
    assert(!guards_intact(std::span<const uint8_t>(a).first(physical-1)));
    std::mt19937 rng(0x42444132);
    for(unsigned trial=0;trial<8192;++trial) {
        Data src,dst;src.fill(sentinel);dst.fill(sentinel);
        for(size_t i=0;i<payload;++i)src[guard+i]=uint8_t(rng());
        auto expected=dst;
        const uint32_t o=4*(rng()%(payload/4+1));
        const uint32_t n=4*(rng()%((payload-o)/4+1));
        std::memcpy(expected.data()+guard+o,src.data()+guard+o,n);
        dispatch(src,dst,{o,n},true);
        assert(dst==expected&&guards_intact(dst));
    }
}
'''
def main():
    p=argparse.ArgumentParser();p.add_argument('--cxx',default='c++');p.add_argument('--sanitize',action='store_true');a=p.parse_args()
    with tempfile.TemporaryDirectory() as t:
        d=Path(t);(d/'test.cpp').write_text(CPP)
        cmd=[a.cxx,'-std=c++20','-O2','-Wall','-Wextra','-Werror','-pedantic',
             '-I',str(ROOT/'vita3k/util/include'),str(d/'test.cpp'),'-o',str(d/'test')]
        if a.sanitize:cmd[2:2]=['-g','-fsanitize=address,undefined','-fno-omit-frame-pointer']
        subprocess.run(cmd,check=True);subprocess.run([str(d/'test')],check=True,timeout=30)
    print('BDA shared ABI/ranges: 8192 seeded copies, every guard byte, dropped copy/return and missing bounds negatives PASS; CPU model, not GPU execution')
if __name__=='__main__':main()
