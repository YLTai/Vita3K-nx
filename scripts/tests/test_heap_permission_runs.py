#!/usr/bin/env python3
"""Exhaustive bounded permission normalization; pure adapter, not Horizon."""
from pathlib import Path
import argparse
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SOURCE = r'''
#include <util/heap_permission_runs.h>
#include <cassert>
#include <array>
#include <cstdint>
using namespace switch_validation::fault;
int main() {
 constexpr uintptr_t base=0x10000;
 std::array<uint32_t,16> perms{};
 uint32_t pinned=0, query_fail=0, single_fail=0; bool deny_batch=false, partial=false;
 unsigned setters=0;
 auto query=[&](uintptr_t p) noexcept {
  auto i=(p-base)/4096;assert(i<16);
  return PagePermission{query_fail&(1u<<i)?0x123u:0u,!(pinned&(1u<<i)),perms[i]};
 };
 auto set=[&](uintptr_t p,size_t bytes,uint32_t value) noexcept -> uint32_t {
  ++setters;const auto first=(p-base)/4096,count=bytes/4096;assert(count&&first+count<=16);
  for(size_t i=first;i<first+count;++i)assert(!(pinned&(1u<<i)));
  for(size_t i=first+1;i<first+count;++i)if(perms[i]!=perms[first])return 0xd401;
  if(deny_batch&&count>1){if(partial)perms[first]=value;return 0xd401;}
  if(count==1&&(single_fail&(1u<<first)))return 0xbeef;
  for(size_t i=first;i<first+count;++i)perms[i]=value;return 0;
 };
 // Every one of the 65536 old R/RW layouts, for both target permissions.
 for(unsigned mask=0;mask<65536;++mask)for(uint32_t target:{1u,3u}){
  unsigned expected_runs=0;bool in=false;
  for(unsigned i=0;i<16;++i){perms[i]=(mask&(1u<<i))?3:1;bool needs=perms[i]!=target;
   if(needs&&!in)++expected_runs;in=needs;}
  setters=0;auto out=normalize_private_pages(base,16*4096,target,query,set);
  assert(out.complete&&!out.failed_mask&&!out.first_rc&&out.queries==32);
  assert(out.before_write_mask==mask&&out.before_read_mask==(65535u^mask));
  assert(out.calls==expected_runs&&setters==expected_runs&&!out.page_retries);
  for(auto p:perms)assert(p==target);
 }
 // A rejected/partially successful range is retried only on verified private pages.
 for(bool p:{false,true}){perms.fill(1);deny_batch=true;partial=p;
  auto out=normalize_private_pages(base,4*4096,3,query,set);
  assert(out.complete&&out.first_rc==0xd401&&out.page_retries==(p?3u:4u));
 }
 deny_batch=false;partial=false;perms.fill(1);pinned=1u<<1;query_fail=1u<<2;
 auto out=normalize_private_pages(base,4*4096,3,query,set);
 assert(!out.complete&&out.failed_mask==6&&perms[0]==3&&perms[3]==3&&perms[1]==1&&perms[2]==1);
 pinned=query_fail=0;perms.fill(1);deny_batch=true;single_fail=1u<<1;
 out=normalize_private_pages(base,4*4096,3,query,set);
 assert(!out.complete&&out.failed_mask==2&&out.first_rc==0xd401&&perms[1]==1);
 setters=0;assert(!normalize_private_pages(base+1,4096,3,query,set).complete);
 assert(!normalize_private_pages(base,0,3,query,set).complete);
 assert(!normalize_private_pages(base,17*4096,3,query,set).complete);
 assert(!normalize_private_pages(base,4096,7,query,set).complete);
 assert(!normalize_private_pages(uintptr_t(-4096),8192,3,query,set).complete);
 assert(!setters);
}
'''

def run(cxx='c++', sanitize=False):
    with tempfile.TemporaryDirectory() as directory:
        d = Path(directory)
        (d/'test.cpp').write_text(SOURCE)
        args=[cxx,'-std=c++20','-O2','-Wall','-Wextra','-Werror','-Wno-misleading-indentation',
              '-I',str(ROOT/'vita3k/util/include'),str(d/'test.cpp'),'-o',str(d/'test')]
        if sanitize: args += ['-fsanitize=address,undefined','-fno-omit-frame-pointer']
        subprocess.run(args,check=True)
        subprocess.run([str(d/'test')],check=True,timeout=20)
    print('Permission normalization: 131072 layouts/targets, mixed-range rejection, partial failure, external refs and bounds passed; adapter test only')

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--cxx',default='c++');p.add_argument('--sanitize',action='store_true')
    a=p.parse_args();run(a.cxx,a.sanitize)
