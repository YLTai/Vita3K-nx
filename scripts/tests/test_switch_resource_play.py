#!/usr/bin/env python3
"""Real normal-play controller/Hash/Table, explicit host platform adapters.
No native A64, GPU execution or game-wide correctness claim.
"""
import argparse,copy,importlib.util,json,subprocess,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
def load(p,n):
 s=importlib.util.spec_from_file_location(n,p);m=importlib.util.module_from_spec(s);s.loader.exec_module(m);return m
MAIN=r'''
int main(int argc,char**argv){
 assert(argc==3);std::filesystem::current_path(argv[1]);std::string scenario=argv[2];
 std::filesystem::create_directories("sdmc:/switch/vita3k");
 // Missing config uses normal play; the benchmark fixture explicitly selects
 // resource_reuse and continues to verify the unchanged six-stage flow.
 if(scenario=="off")std::ofstream("sdmc:/switch/vita3k/tracelab.cfg")<<"mode=off\n";
 else if(scenario=="explicit")std::ofstream("sdmc:/switch/vita3k/tracelab.cfg")<<"mode=auto\nvalidation_experiment=resource_reuse_play\n";
 sv::begin_session("PLAY_FIXTURE");
 if(scenario=="off"){assert(!sv::resource_reuse_enabled());sv::on_render_end(10000);assert(!sv::enabled());return 0;}
 sv::direct_heap::state.active=true;sv::deferred_import::registry.configure(true);sv::resource_backend(true);
 for(auto name:sv::compact_controls)sv::record("write_watch_canary",name,"pass","explicit host adapter");
 auto raw=std::make_unique<uint8_t*[]>(1<<20);
 auto*data=static_cast<uint8_t*>(std::aligned_alloc(4096,16384));assert(data);std::memset(data,0,16384);
 for(unsigned p=2;p<6;++p)sv::live_publish_page(raw.get(),p,reinterpret_cast<uint8_t*>(uintptr_t(data)+(p-2)*4096-p*4096));
 sv::live_publish_page(raw.get(),8,reinterpret_cast<uint8_t*>(uintptr_t(data)-32768));
 auto**writes=sv::live_write_table(raw.get());assert(writes);
 MemState mem{{raw.get()},data+8192};
 auto get=[&](unsigned page=2){SceGxmTexture tex{page*1024,4};return hash_texture_data(tex,4096,mem);};
 auto expected=[&](unsigned page=2){return XXH3_64bits(data+(page-2)*4096,4096)^XXH3_64bits(data+8192,64);};
 uint64_t tick=10000;auto frame=[&]{tick+=100;sv::on_render_end(tick);};
 get();frame();assert(sv::status_snapshot().resource_play);
 // Register late input, then publish guards on the actual renderer callback.
 get();frame();assert(!writes[2]&&!writes[8]);
 auto cold=reads;assert(get()==expected()&&reads>cold);
 if(scenario=="missing")data[31]^=11; // after baseline, before mandatory entry audit
 auto auditing=reads;assert(get()==expected()&&reads>auditing);frame();
 if(scenario=="missing"){
  auto s=sv::status_snapshot();assert(s.reuse_diffs==1&&s.watch_failed&&writes[2]);
  auto n=reads;assert(get()==expected()&&reads>n);
 }else{
  auto n=reads;assert(get()==expected()&&reads==n); // first real no-scan reuse
  for(unsigned i=0;i<10000;++i){assert(get()==expected());}
  assert(reads==n);
  // A new source discovered during normal play gains guards, full baseline,
  // and an independent audit before it can be reused. No fixed scene timer.
  std::atomic<bool> loaded=false,release=false;
  std::thread old_jit([&]{sv::epoch::RunLease run;auto*saved=raw[5]+5*4096;loaded=true;
   while(!release.load()){std::this_thread::yield();}saved[0]=23;});
  while(!loaded.load()){std::this_thread::yield();}
  get(5);frame();assert(!writes[5]&&!writes[2]);
  n=reads;get(5);get(5);assert(reads==n+4); // old raw PTE still held: no baseline authority
  release=true;old_jit.join();
  assert(get(5)==expected(5));n=reads;get(5);assert(reads>n);n=reads;get(5);assert(reads==n);
  // Unknown or exact writer changes cannot be bypassed by automatic mode.
  uint32_t v=19;assert(sv::live_write(raw.get(),32768,&v,4)==sv::watch::Access::Complete);
  n=reads;assert(get()==expected()&&reads>n);
  {sv::epoch::HostOutputLease h(data+10,{1,true});data[10]^=7;n=reads;assert(get()==expected()&&reads>n);}
  assert(get()==expected());
  {sv::epoch::HostLease unknown;data[11]^=9;}n=reads;assert(get()==expected()&&reads>n);
  // Same allocation with withdrawn mapping requires a new baseline AND audit.
  sv::live_publish_page(raw.get(),2,nullptr);sv::live_publish_page(raw.get(),2,reinterpret_cast<uint8_t*>(uintptr_t(data)-8192));
  n=reads;get();assert(reads>n);frame();get();n=reads;get();assert(reads>n);n=reads;get();assert(reads==n);
  // Pause/menu and the old capture shortcut never reset qualification/history.
  sv::frontend_poll(true);frame();assert(!sv::status_snapshot().watch_failed);
  sv::frontend_poll(false);frame();n=reads;get();assert(reads==n);
  sv::request_benchmark();frame();assert(sv::status_snapshot().resource_play&&sv::status_snapshot().phase_count==0);
  // GPU writes stay sticky and must never be admitted, even with equal bytes.
  sv::resource_gpu_write(data+12288,4096);n=reads;get(5);get(5);assert(reads==n+4);
  if(scenario=="budget"){
   for(unsigned i=0;i<100000;++i){uint32_t value=i;sv::live_write(raw.get(),32768,&value,4);}
   frame();assert(sv::status_snapshot().reuse_blockers==4&&writes[2]);
   n=reads;assert(get()==expected()&&reads>n);
  }
  if(scenario=="block"){
   sv::resource_deny(32);frame();auto s=sv::status_snapshot();assert(s.stage==sv::Stage::Stopped&&s.reuse_blockers==32&&writes[2]);
   n=reads;get();assert(reads>n);sv::request_benchmark();frame();assert(sv::status_snapshot().reuse_blockers==32);
  }
  if(scenario=="failure"){
   sv::record("fixture","fail","fail","deliberate control failure");frame();assert(sv::status_snapshot().watch_failed&&writes[2]);
  }
 }
 auto view=sv::status_snapshot();auto hud=sv::make_hud_text(view,tick);assert(std::strstr(hud.action.data(),"Play normally"));
 assert(!std::strstr(hud.timing.data(),"Stage"));
 std::filesystem::create_directory("report");sv::write_report("report");
 sv::live_detach_table(raw.get());sv::end_session();std::free(data);
}
'''
def main():
 p=argparse.ArgumentParser();p.add_argument('--cxx',default='c++');p.add_argument('--sanitize',action='store_true');p.add_argument('--require-source',action='store_true');a=p.parse_args()
 if a.require_source:subprocess.run(['python3',str(ROOT/'scripts/apply-switch-write-watch-hooks.py'),'--check-applied'],check=True)
 base=load(ROOT/'scripts/tests/test_switch_resource_integration.py','base_play')
 cpp=base.CPP.split('// Only the actual worker')[0]+MAIN
 prod=(ROOT/'vita3k/renderer/src/texture/cache.cpp').read_text();start=prod.index('uint64_t hash_texture_data(');end=prod.index('\n}',start)+2
 cpp=cpp.replace('// PRODUCTION_HASH_BODY',prod[start:end])
 xx=ROOT/'external/xxHash/xxhash.h';flags=['-DPINNED_HASH','-I'+str(xx.parent)] if xx.exists() else ['-lxxhash']
 if a.require_source and not xx.exists():raise RuntimeError('CI must use pinned XXH3')
 analyzer=load(ROOT/'scripts/analyze-switch-resource-reuse.py','play_analysis')
 with tempfile.TemporaryDirectory() as tmp:
  d=Path(tmp);inc=d/'include';(inc/'util').mkdir(parents=True);(inc/'switch/arm').mkdir(parents=True)
  (inc/'util/log.h').write_text('#pragma once\n#define LOG_INFO(...) ((void)0)\n#define LOG_WARN(...) ((void)0)\n#define LOG_ERROR(...) ((void)0)\n')
  (inc/'switch/arm/counter.h').write_text('#pragma once\n#include <cstdint>\ninline uint64_t armGetSystemTickFreq(){return 1000;}\n')
  (d/'main.cpp').write_text(cpp)
  cmd=[a.cxx,'-std=c++20','-O0' if a.sanitize else '-O2','-Wall','-Wextra','-Werror','-pthread','-D__SWITCH__','-DVITA3K_SWITCH_TRACELAB=1','-DVITA3K_SWITCH_RESOURCE_REUSE_DEFAULT=1','-DVITA3K_SWITCH_CLEAN_CAPTURE_DEFAULT=1','-DVITA3K_SWITCH_EPOCH_AUDIT_DEFAULT=1','-I'+str(inc),'-I'+str(ROOT/'vita3k/util/include'),str(d/'main.cpp'),str(ROOT/'vita3k/util/src/switch_memory_validation.cpp'),*flags,'-o',str(d/'play')]
  if a.sanitize:cmd+=['-fsanitize=address,undefined','-fno-omit-frame-pointer']
  subprocess.run(cmd,check=True,timeout=90)
  for scenario in ('default','explicit','missing','block','budget','failure','off'):
   out=d/scenario;out.mkdir();subprocess.run([str(d/'play'),str(out),scenario],check=True,timeout=45)
   if scenario=='off':continue
   doc=json.loads((out/'report/memory-validation.json').read_text());r=analyzer.analyze(doc)
   assert r['comparison_eligible'] is False
   if scenario in ('missing','failure'):assert not r['evidence_valid'],r
   else:assert r['evidence_valid'],r
   if scenario=='default':clean=doc;assert r['actual_work']['reused_calls']>=10000
   print(scenario,r['actual_work']['reused_calls'],r['actual_work']['audits'])
  if not a.sanitize:
   # Compile the real headers with deliberately broken gates. Each must fail
   # at runtime, not because a source-identity grep noticed the mutation.
   for name,path,old,new in [
    ('no-entry-audit','resource_hash_reuse.h','(mode == Mode::Online && inherited_audit)','(mode == Mode::Online)'),
    ('no-late-guards','switch_write_watch.h','refresh_pending_ = false;\n        try {','refresh_pending_ = false; return true;\n        try {')]:
    mutant=d/name/'util';mutant.mkdir(parents=True)
    source=(ROOT/'vita3k/util/include/util'/path).read_text();assert source.count(old)==1
    (mutant/path).write_text(source.replace(old,new,1))
    badcmd=cmd.copy();badcmd.insert(1,'-I'+str(mutant.parent))
    subprocess.run(badcmd,check=True,timeout=90)
    out=d/(name+'-run');out.mkdir();result=subprocess.run([str(d/'play'),str(out),'default'],capture_output=True,timeout=30)
    assert result.returncode<0 and b'Assertion' in result.stderr,(name,result.stderr)
  for field,value in [('first_reuse_requires_full_audit',False),('GPU_write_sources_reused',True),('scene_memo','none')]:
   bad=copy.deepcopy(clean);bad['resource_reuse'][field]=value;assert not analyzer.analyze(bad)['evidence_valid']
  bad=copy.deepcopy(clean);bad['resource_reuse']['total']['reused_calls']+=1;assert not analyzer.analyze(bad)['evidence_valid']
 print('PASS: default/explicit/off, late inputs, automatic full baseline+audit before reuse, 10000 no-read hits, actual alias/HLE/unknown/remap fallback, sticky GPU/network exclusions, pause, ignored benchmark shortcut, control failure, audit mismatch, HUD and JSON. Host adapters, not device execution.')
if __name__=='__main__':main()
