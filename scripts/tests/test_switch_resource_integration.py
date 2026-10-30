#!/usr/bin/env python3
"""Run actual integration controller/Table/HashCache/HUD/export against XXH3.
Kernel/renderer inputs are explicit adapters; this is not Horizon/GPU execution.
"""
import argparse,copy,hashlib,importlib.util,json,os,subprocess,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
CPP=r'''
#include <util/switch_memory_validation.h>
#include <util/switch_resource_reuse.h>
#include <util/switch_write_watch_api.h>
#include <util/switch_direct_heap.h>
#include <util/switch_capture_plan.h>
#include <util/host_output_scope.h>
#include <util/resource_source_policy.h>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#ifdef PINNED_HASH
#define XXH_INLINE_ALL
#include <xxhash.h>
#else
extern "C" uint64_t XXH3_64bits(const void *,size_t);
#endif
namespace sv=switch_validation;
static uint64_t reads=0;
uint64_t hash(const void*p,size_t n){++reads;return XXH3_64bits(p,n);}

#include <util/switch_content_compare.h>
namespace switch_validation {
void run_protected_cache_probe(ResourceHash) { assert(false); }
namespace direct_heap { void observe(const void *,uint64_t) { assert(false); } }
}
struct MemState { struct Pages { uint8_t **value;uint8_t **get() const {return value;} } page_table;uint8_t *palette; };
struct SceGxmTexture {uint32_t data_addr;int fmt=4;};
using SceGxmTextureFormat=int;using SceGxmTextureBaseFormat=int;
constexpr int SCE_GXM_TEXTURE_BASE_FORMAT_P4=4,SCE_GXM_TEXTURE_BASE_FORMAT_P8=8;
namespace gxm {int get_format(const SceGxmTexture&t){return t.fmt;}int get_base_format(int f){return f;}}
template<class T> struct Ptr {uint32_t a;uint32_t address()const{return a;}const void*get(const MemState&m)const{return m.page_table.get()[a>>12]+a;}};
bool is_valid_addr_range(const MemState&,uint32_t a,uint32_t b){return b>=a;}
const uint32_t*get_texture_palette(const SceGxmTexture&,const MemState&m){return reinterpret_cast<const uint32_t*>(m.palette);}
uint64_t hash_data(const void*p,size_t n){return hash(p,n);}
uint64_t hash_palette_data(const SceGxmTexture&t,size_t n,const MemState&m){return hash_data(get_texture_palette(t,m),n*4);}
namespace tracelab{enum class Metric{TextureHash};enum class Domain{Render};struct Scope{Scope(Metric,Domain,uint64_t){}};}
#define LOG_WARN_ONCE(...) ((void)0)
// PRODUCTION_HASH_BODY

int main(int argc,char**argv){
 assert(argc==3);std::filesystem::current_path(argv[1]);
 const std::string scenario=argv[2];
 std::filesystem::create_directories("sdmc:/switch/vita3k");
 std::ofstream("sdmc:/switch/vita3k/tracelab.cfg")<<"mode=auto\nvalidation_experiment=resource_reuse\n";
 sv::begin_session("RESOURCE_FIXTURE");
 sv::direct_heap::state.active=true;sv::deferred_import::registry.configure(true);sv::resource_backend(true);
 for(auto name:sv::compact_controls)sv::record("write_watch_canary",name,"pass","host fixture for controller, not device control evidence");
 auto raw=std::make_unique<uint8_t*[]>(1<<20);
 auto*data=static_cast<uint8_t*>(std::aligned_alloc(4096,12288));assert(data);std::memset(data,0,12288);
 for(unsigned page=2;page<5;++page)sv::live_publish_page(raw.get(),page,reinterpret_cast<uint8_t*>(uintptr_t(data)+(page-2)*4096-page*4096));
 sv::live_publish_page(raw.get(),8,reinterpret_cast<uint8_t*>(uintptr_t(data)-8*4096));
 auto**writes=sv::live_write_table(raw.get());assert(writes);
 sv::resource_gpu_write(data+4096,4096); // excluded GPU target before any source registration
 MemState mem{{raw.get()},data+8192};
 auto get=[&](bool excluded=false){SceGxmTexture texture{(excluded?12288u:8192u)>>2,4};return hash_texture_data(texture,4096,mem);};
 auto expected=[&]{return XXH3_64bits(data,4096)^XXH3_64bits(data+8192,64);};
 get();uint64_t tick=10000;sv::on_render_end(tick);assert(!sv::strict_hash());sv::request_benchmark();sv::on_render_end(tick);
 unsigned old=0,n=0;bool injected=false,saw_skip=false,saw_veto=false;
 for(unsigned frame=0;frame<700;++frame){
  auto view=sv::status_snapshot();if(view.stage==sv::Stage::Complete)break;
  const unsigned phase=view.phase-1;if(phase!=old){old=phase;n=0;}
  assert(view.resource_reuse&&view.phase_count==6);assert(sv::strict_hash()==(phase==1||phase==4));
  const auto title=sv::make_hud_text(view,tick);assert(std::strstr(title.title.data(),"RESOURCE REUSE"));
  if(phase==2&&n==5){uint32_t v=7;assert(!writes[2]&&!writes[8]);assert(sv::live_write(raw.get(),32768,&v,4)==sv::watch::Access::Complete);}
  if(phase==3&&n==5){sv::epoch::HostOutputLease write(data+3,{1,true});data[3]^=4;const auto before=reads;assert(get()==expected()&&reads>before);}
  if(phase==3&&n==10){sv::epoch::HostLease unknown;data[20]^=9;}
  if(scenario=="missing"&&phase==4&&n==15){data[39]^=11;injected=true;}
  if(scenario=="veto"&&phase==2&&n==12){sv::resource_gpu_write(data,4096);saw_veto=true;}
  if(scenario=="remap"&&phase==3&&n==18){sv::live_publish_page(raw.get(),2,nullptr);sv::live_publish_page(raw.get(),2,reinterpret_cast<uint8_t*>(uintptr_t(data)-8192));}
  if(scenario=="denied"&&phase==2&&n==12)sv::resource_deny(1);
  for(unsigned k=0;k<4;++k){const auto before=reads;assert(get()==expected());if(reads==before){assert(phase==2||phase==3);assert(!saw_veto);saw_skip=true;}}
  const auto before=reads;get(true);assert(reads>before); // GPU target never has a fast hit
  tick+=scenario=="slow"?1000:100;sv::on_render_end(tick);++n;
 }
 auto view=sv::status_snapshot();assert(view.stage==sv::Stage::Complete);assert(writes[2]);assert(!sv::strict_hash());
 if(scenario=="missing"){assert(injected&&view.outcome==sv::CaptureOutcome::Invalid&&view.reuse_diffs==1);}
 else {assert(view.reuse_diffs==0);assert(saw_skip);if(scenario=="slow")assert(view.outcome==sv::CaptureOutcome::Inconclusive);else if(scenario=="clean")assert(view.outcome==sv::CaptureOutcome::Ready);}
 const auto final=sv::make_hud_text(view,tick);assert(std::strstr(final.title.data(),"DONE"));
 sv::frontend_poll(true);assert(sv::status_snapshot().stage==sv::Stage::Complete);
 std::filesystem::create_directory("report");sv::write_report("report");sv::live_detach_table(raw.get());sv::end_session();std::free(data);
}
'''
def load(path,name):
 s=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(s);s.loader.exec_module(m);return m

def main():
 p=argparse.ArgumentParser();p.add_argument('--cxx',default='c++');p.add_argument('--sanitize',action='store_true');p.add_argument('--require-source',action='store_true');a=p.parse_args()
 engine=load(ROOT/'scripts/apply-switch-write-watch-hooks.py','apply_hooks')
 if a.require_source:
  subprocess.run(['python3',str(ROOT/'scripts/apply-switch-write-watch-hooks.py'),'--check-applied'],check=True)
  recipe=json.loads((ROOT/'patches/experiments/switch-resource-integration.json').read_text())
  for path,sha in recipe['reviewed_dependencies'].items():
   blob=subprocess.run(['git','show','HEAD:'+path],cwd=ROOT,capture_output=True,check=True).stdout
   assert engine.blob_sha(blob)==sha,('unreviewed native producer',path)
 xx=ROOT/'external/xxHash/xxhash.h';hash_flags=['-DPINNED_HASH','-I'+str(xx.parent)] if xx.exists() else ['-lxxhash']
 if a.require_source and not xx.exists():raise RuntimeError('CI requires pinned xxHash')
 analyzer=load(ROOT/'scripts/analyze-switch-resource-reuse.py','reuse_analyzer')
 with tempfile.TemporaryDirectory() as tmp:
  d=Path(tmp);inc=d/'include';(inc/'util').mkdir(parents=True);(inc/'switch/arm').mkdir(parents=True)
  (inc/'util/log.h').write_text('#pragma once\n#define LOG_INFO(...) ((void)0)\n#define LOG_WARN(...) ((void)0)\n#define LOG_ERROR(...) ((void)0)\n')
  (inc/'switch/arm/counter.h').write_text('#pragma once\n#include <cstdint>\ninline uint64_t armGetSystemTickFreq(){return 1000;}\n')
  source=(ROOT/'vita3k/renderer/src/texture/cache.cpp').read_text();start=source.index('uint64_t hash_texture_data(');end=source.index('\n}',start)+2
  (d/'main.cpp').write_text(CPP.replace('// PRODUCTION_HASH_BODY',source[start:end]))
  cmd=[a.cxx,'-std=c++20','-O0' if a.sanitize else '-O2','-Wall','-Wextra','-Werror','-pthread','-D__SWITCH__','-DVITA3K_SWITCH_TRACELAB=1','-DVITA3K_SWITCH_RESOURCE_REUSE_DEFAULT=1','-DVITA3K_SWITCH_CLEAN_CAPTURE_DEFAULT=1','-DVITA3K_SWITCH_EPOCH_AUDIT_DEFAULT=1','-I'+str(inc),'-I'+str(ROOT/'vita3k/util/include'),str(d/'main.cpp'),str(ROOT/'vita3k/util/src/switch_memory_validation.cpp'),*hash_flags,'-o',str(d/'controller')]
  if a.sanitize:cmd+=['-fsanitize=address,undefined','-fno-omit-frame-pointer']
  subprocess.run(cmd,check=True,timeout=90)
  for scenario in ['clean','missing','veto','remap','denied','slow']:
   out=d/scenario;out.mkdir();subprocess.run([str(d/'controller'),str(out),scenario],check=True,timeout=45)
   data=json.loads((out/'report/memory-validation.json').read_text());report=analyzer.analyze(data)
   if scenario=='clean':
    assert report['comparison_eligible'],report;clean=data
   elif scenario=='missing':assert report['exclusions'] and report['actual_work']['mismatches']==1,report
   elif scenario=='slow':assert not report['comparison_eligible'] and not report['exclusions'],report
   else:assert not report['exclusions'],report
   print(scenario,report['actual_work']['reused_calls'],report['actual_work']['reference_calls'],flush=True)
  for field,value in [('production_path',False),('GPU_write_sources_reused',True),('scene_memo','audit_only')]:
   bad=copy.deepcopy(clean);bad['resource_reuse'][field]=value;assert analyzer.analyze(bad)['exclusions']
  bad=copy.deepcopy(clean);bad['resource_reuse']['total']['reused_calls']+=1;assert analyzer.analyze(bad)['exclusions']
 print('PASS: real integration controller/Table/cache/HUD/JSON, alias store, host output, unknown writer, persistent GPU veto, remap, denied profile, slow cap and missing-writer negative; no native/GPU execution claim')
if __name__=='__main__':main()
