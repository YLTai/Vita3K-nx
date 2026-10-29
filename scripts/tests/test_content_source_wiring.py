#!/usr/bin/env python3
"""Compile the actual hash_texture_data body with minimal GXM/trace fixtures.
CI reads the fully patched checkout. --snippet is for an explicitly fetched
unaltered function body in a partial checkout, not a full recipe proof.
"""
import argparse
import json
from pathlib import Path
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[2]
HEAD=r'''
#include <util/content_hash_memo.h>
#include <util/switch_direct_heap_platform.h>
#include <util/switch_protected_cache_probe.h>
#include <array>
#include <cassert>
#include <memory>
using SceGxmTextureFormat=int;
using SceGxmTextureBaseFormat=int;
constexpr int SCE_GXM_TEXTURE_BASE_FORMAT_P4=4,SCE_GXM_TEXTURE_BASE_FORMAT_P8=8;
struct SceGxmTexture{uint32_t data_addr=1;int fmt=0;};
struct MemState{std::array<uint8_t,8192> pixels{};std::array<uint32_t,256> palette{};bool valid=true;std::unique_ptr<uint8_t*[]> page_table;};
namespace gxm{int get_format(const SceGxmTexture&t){return t.fmt;}int get_base_format(int f){return f;}}
template<class T>struct Ptr{uint32_t a;uint32_t address()const{return a;}const void*get(const MemState&m)const{return m.pixels.data();}};
bool is_valid_addr_range(const MemState&m,uint32_t,uint32_t){return m.valid;}
const uint32_t*get_texture_palette(const SceGxmTexture&,const MemState&m){return m.palette.data();}
uint64_t hash_data(const void*p,size_t n){uint64_t h=1469598103934665603ULL;for(size_t i=0;i<n;++i){h^=static_cast<const uint8_t*>(p)[i];h*=1099511628211ULL;}return h;}
uint64_t hash_palette_data(const SceGxmTexture&t,size_t n,const MemState&m){return hash_data(get_texture_palette(t,m),n*4);}
namespace tracelab{enum class Metric{TextureHash};enum class Domain{Render};struct Scope{Scope(Metric,Domain,uint32_t){}};}
#define LOG_WARN_ONCE(...) ((void)0)
namespace switch_validation{
content::Memo memo;content::Counts stats;content::Mode mode=content::Mode::Full;
bool content_compare_enabled(){return true;}
uint64_t content_hash(const content::Input&i,content::Hash h){return memo.evaluate(i,mode,h,stats);}
struct Token{};
Token live_hash_begin(uint8_t**,uint32_t,const void*,uint64_t,const void*,uint64_t){assert(false);return {};}
void live_hash_end(Token,uint64_t){}
}
'''
MAIN=r'''
int main(){
 MemState mem;SceGxmTexture t;
 for(int fmt:{0,4,8}){
  t.fmt=fmt;
  auto expected=[&]{return hash_data(mem.pixels.data(),mem.pixels.size())^(fmt?hash_data(mem.palette.data(),fmt==4?64:1024):0);};
  for(auto mode:{switch_validation::content::Mode::Full,switch_validation::content::Mode::Audit,switch_validation::content::Mode::Compare}){
   switch_validation::mode=mode;
   for(int k=0;k<12;++k){if(k==3)mem.pixels.back()^=1;if(k==7)mem.palette[fmt==4?15:255]^=1;
    assert(hash_texture_data(t,mem.pixels.size(),mem)==expected());}
  }
 }
 mem.valid=false;assert(hash_texture_data(t,8192,mem)==0);
 mem.valid=true;t.data_addr=0;t.fmt=8;assert(hash_texture_data(t,8192,mem)==hash_data(mem.palette.data(),1024));
}
'''
def main():
 p=argparse.ArgumentParser();p.add_argument('--cxx',default='c++');p.add_argument('--snippet',type=Path);a=p.parse_args()
 source=(a.snippet or ROOT/'vita3k/renderer/src/texture/cache.cpp').read_text()
 start=source.index('uint64_t hash_texture_data(');end=source.index('\n}',start)+2;body=source[start:end]
 if 'content_compare_enabled()' not in body:
  recipe=json.loads((ROOT/'patches/experiments/switch-live-write-watch.json').read_text())
  spec=next(f for f in recipe['files'] if f['path'].endswith('/cache.cpp'))
  for r in spec['replacements']:
   if r['old'] in body:
    assert body.count(r['old'])==1;body=body.replace(r['old'],r['new'],1)
 assert 'content_compare_enabled()' in body and 'size_t{texture_size}' in body
 with tempfile.TemporaryDirectory() as d:
  d=Path(d);(d/'main.cpp').write_text(HEAD+body+MAIN)
  subprocess.run([a.cxx,'-std=c++20','-O2','-Wall','-Wextra','-Werror','-pedantic','-I',str(ROOT/'vita3k/util/include'),str(d/'main.cpp'),'-o',str(d/'test')],check=True)
  subprocess.run([str(d/'test')],check=True)
 print('Actual hash_texture_data body: ordinary/P4/P8, full/audit/compare, invalid/null sources passed; '+('fetched partial function (whole-blob CI still required)' if a.snippet else 'checkout integration'))
if __name__=='__main__':main()
