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
 // A normal/default config must not start the globally instrumented mode.
 const bool inactive=scenario=="default"||scenario=="off"||scenario=="validation-off"||scenario=="old-auto";
 if(scenario=="off")std::ofstream("sdmc:/switch/vita3k/tracelab.cfg")<<"mode=off\nvalidation_experiment=resource_reuse_play\n";
 else if(scenario=="validation-off")std::ofstream("sdmc:/switch/vita3k/tracelab.cfg")<<"memory_validation=off\nvalidation_experiment=resource_reuse_play\n";
 else if(scenario=="old-auto")std::ofstream("sdmc:/switch/vita3k/tracelab.cfg")<<"mode=auto\nmemory_validation=on\n";
 else if(scenario!="default")std::ofstream("sdmc:/switch/vita3k/tracelab.cfg")<<"mode=auto\nvalidation_experiment=resource_reuse_play\n";
 sv::begin_session("PLAY_FIXTURE");
 if(inactive){
  assert(!sv::resource_reuse_enabled()&&!sv::enabled()&&!sv::epoch::enabled.load());
  auto table=std::make_unique<uint8_t*[]>(1<<20);alignas(4096) uint8_t bytes[4096]{};
  auto entry=reinterpret_cast<uint8_t*>(uintptr_t(bytes)-8192);sv::live_publish_page(table.get(),2,entry);
  assert(table[2]==entry&&!sv::live_write_table(table.get()));
  auto before=sv::epoch::domain.snapshot();
  for(unsigned i=0;i<10000;++i){sv::epoch::RunLease run;sv::epoch::HostLease host;sv::epoch::HostOutputLease output(bytes,{1,true});sv::epoch::gpu_boundary();}
  auto after=sv::epoch::domain.snapshot();assert(before.entries==after.entries&&before.host_entries==after.host_entries&&before.gpu_boundaries==after.gpu_boundaries);
  MemState mem{{table.get()},bytes};SceGxmTexture tex{2048,4};auto n=reads;
  assert(hash_texture_data(tex,4096,mem)==(XXH3_64bits(bytes,4096)^XXH3_64bits(bytes,64))&&reads==n+2);
  sv::request_benchmark();sv::on_render_end(10000);assert(!sv::enabled());sv::end_session();return 0;
 }
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
  sv::resource_gpu_write(data+4096,4096); // excluded BEFORE first registration
  n=reads;for(unsigned i=0;i<1024;++i)get(3);assert(reads==n+2048);
  frame();assert(writes[3]==raw[3]); // no pointless selected-page trap
  get();get();
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
  if(scenario=="drain"){
   std::atomic<bool>ready=false,release=false;
   std::thread writer([&]{sv::epoch::RunLease run;sv::epoch::HostLease host;
    sv::epoch::HostOutputLease output(data+32,{1,true});ready=true;
    while(!release.load())std::this_thread::yield();
    data[32]^=3;
   });
   while(!ready.load())std::this_thread::yield();
   auto live=sv::epoch::domain.snapshot();assert(live.active==1&&live.host_active==1);
   sv::resource_deny(32);frame();assert(!sv::epoch::enabled.load());
   // Retirement must not reset in-flight reader/output metadata.
   live=sv::epoch::domain.snapshot();assert(live.active==1&&live.host_active==1);
   release=true;writer.join();live=sv::epoch::domain.snapshot();
   assert(!live.failed&&!live.active&&!live.host_active&&sv::epoch::domain.output_stats().active==0);
  }
  if(scenario=="failure"){
   sv::record("fixture","fail","fail","deliberate control failure");frame();assert(sv::status_snapshot().watch_failed&&writes[2]);
  }
 }
 auto view=sv::status_snapshot();
 if(view.stage==sv::Stage::Stopped){
  assert(view.reuse_tracking_retired&&!sv::resource_reuse_enabled()&&!sv::epoch::enabled.load());
  assert(sv::enabled()); // evidence/UI survives retirement
  assert(!sv::live_write_table(raw.get())); // new JITs use original write table
  for(unsigned page:{2u,3u,4u,5u,8u})assert(writes[page]==raw[page]);
  auto before=sv::epoch::domain.snapshot();auto outputs=sv::epoch::domain.output_stats();
  for(unsigned i=0;i<10000;++i){sv::epoch::RunLease run;sv::epoch::HostLease host;sv::epoch::HostOutputLease output(data,{1,true});sv::epoch::gpu_boundary();}
  auto after=sv::epoch::domain.snapshot();assert(before.entries==after.entries&&before.host_entries==after.host_entries&&before.gpu_boundaries==after.gpu_boundaries);
  assert(outputs.begins==sv::epoch::domain.output_stats().begins&&!after.failed);
  uint32_t value=77,dst=99;bool success=true;
  assert(sv::live_read(raw.get(),8192,&dst,4)==sv::watch::Access::Unhandled&&dst==99);
  assert(sv::live_write(raw.get(),8192,&value,4)==sv::watch::Access::Unhandled);
  assert(sv::live_exclusive(raw.get(),8192,7,0,4,success)==sv::watch::Access::Unhandled);
  // Existing X25 mirrors must still receive map/unmap updates after retirement.
  sv::live_publish_page(raw.get(),2,reinterpret_cast<uint8_t*>(uintptr_t(data+12288)-8192));
  assert(writes[2]==raw[2]);sv::live_publish_page(raw.get(),8,nullptr);assert(!writes[8]);
  sv::request_benchmark();frame();assert(!sv::resource_reuse_enabled());
 }
 auto hud=sv::make_hud_text(view,tick);assert(std::strstr(hud.action.data(),"Play normally"));
 if(view.stage==sv::Stage::Stopped)assert(std::strstr(hud.timing.data(),"TRACKING OFF"));
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
 # Real production implementations are gated by heap_fault/active topology.
 # The fixture has no topology window; after retirement the ordinary path must
 # pass these disabled entry points without reviving an observer.
 cpp=cpp.replace('void run_protected_cache_probe(ResourceHash) { assert(false); }',
                 'void run_protected_cache_probe(ResourceHash) { assert(!resource_reuse_enabled()); }')
 cpp=cpp.replace('void observe(const void *,uint64_t) { assert(false); }',
                 'void observe(const void *,uint64_t) { assert(!resource_reuse_enabled()); }')
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
  for scenario in ('default','old-auto','explicit','missing','block','budget','failure','drain','validation-off','off'):
   out=d/scenario;out.mkdir();subprocess.run([str(d/'play'),str(out),scenario],check=True,timeout=45)
   if scenario in ('default','old-auto','validation-off','off'):continue
   doc=json.loads((out/'report/memory-validation.json').read_text());r=analyzer.analyze(doc)
   assert r['comparison_eligible'] is False
   if scenario in ('missing','failure'):assert not r['evidence_valid'],r
   else:assert r['evidence_valid'],r
   if scenario=='explicit':clean=doc;assert r['actual_work']['reused_calls']>=10000
   if scenario in ('missing','block','budget','failure','drain'):
    retired=doc['resource_reuse']['retirement'];assert retired['tracking_retired'] is True
    assert retired['stop_tick']>0
    bad=copy.deepcopy(doc);bad['resource_reuse']['retirement']['tracking_retired']=False
    assert not analyzer.analyze(bad)['evidence_valid']
    if scenario=='budget':assert retired['reason']=='write_budget' and retired['watched_stores']>=100000
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
    badcmd=cmd.copy();badcmd.insert(1,'-I'+str(mutant.parent));badcmd+=['-O0']
    subprocess.run(badcmd,check=True,timeout=90)
    out=d/(name+'-run');out.mkdir();result=subprocess.run([str(d/'play'),str(out),'explicit'],capture_output=True,timeout=30)
    assert result.returncode<0 and b'Assertion' in result.stderr,(name,result.stderr)
  if not a.sanitize:
   controller_path=ROOT/'vita3k/util/src/switch_memory_validation.cpp'
   controller=controller_path.read_text()
   # Runtime negatives exercise the actual controller, not a model or grep.
   for name,old,new,scenario in [
    ('leaked-domain','epoch::enabled.store(false, std::memory_order_release);\n                watch_runtime_active.store(false, std::memory_order_release);','watch_runtime_active.store(false, std::memory_order_release);','block'),
    ('implicit-play','if (out.resource_reuse && !explicit_resource_profile) out.enabled = false;','if (out.resource_reuse && !explicit_resource_profile) out.enabled = true;','default'),
    ('stale-mirror','if (!enabled() || !options.write_shadow) { normal[page] = entry; return; }','if (!enabled() || !options.write_shadow || !watch_runtime_active.load()) { normal[page] = entry; return; }','block')]:
    assert controller.count(old)==1
    broken=d/(name+'.cpp');broken.write_text(controller.replace(old,new,1))
    badcmd=[str(broken) if x==str(controller_path) else x for x in cmd]+['-O0']
    subprocess.run(badcmd,check=True,timeout=90)
    out=d/(name+'-run');out.mkdir()
    result=subprocess.run([str(d/'play'),str(out),scenario],capture_output=True,timeout=30)
    assert result.returncode<0 and b'Assertion' in result.stderr,(name,result.stderr)
  for field,value in [('first_reuse_requires_full_audit',False),('GPU_write_sources_reused',True),('scene_memo','none')]:
   bad=copy.deepcopy(clean);bad['resource_reuse'][field]=value;assert not analyzer.analyze(bad)['evidence_valid']
  bad=copy.deepcopy(clean);bad['resource_reuse']['total']['reused_calls']+=1;assert not analyzer.analyze(bad)['evidence_valid']
 print('PASS: default-off/explicit/off, drained terminal tracker, 10000 post-stop leases cause no entries, live mirror remap, no vetoed-source registration, late inputs, automatic full baseline+audit before reuse, 10000 no-read hits, actual alias/HLE/unknown/remap fallback, sticky GPU/network exclusions, pause, ignored benchmark shortcut, control failure, audit mismatch, HUD and JSON. Host adapters, not device execution.')
if __name__=='__main__':main()
