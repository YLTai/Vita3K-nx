#!/usr/bin/env python3
"""Real observer + grace-domain/controller regression. Host stores model JIT dispatch.
No Horizon, native A64 or GPU execution is claimed by these tests.
"""
import argparse
import copy
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[2]
PURE = r'''
#include <util/switch_write_watch.h>
#include <cassert>
#include <future>
#include <thread>
#include <vector>
#include <cstdlib>
using namespace switch_validation;
int main() {
 epoch::Domain d; watch::Table table; table.set_epoch_domain(&d);
 auto raw=std::make_unique<uint8_t*[]>(watch::page_count);
 auto *data=static_cast<uint8_t*>(std::aligned_alloc(4096,8192)); assert(data);
 std::memset(data,0,8192);
 raw[2]=reinterpret_cast<uint8_t*>(uintptr_t(data)-8192);
 raw[4]=reinterpret_cast<uint8_t*>(uintptr_t(data)-16384);
 auto **writes=table.bind(raw.get()); assert(writes);
 table.phase(0,false,true);
 auto hash=[&] { auto token=table.before(raw.get(),8192,data,4); uint32_t h;std::memcpy(&h,data,4);return table.after(token,h); };
 hash();
 std::promise<void> loaded,release;auto ready=loaded.get_future();auto go=release.get_future();
 std::thread old([&]{epoch::RunLease lease(&d);auto *saved=raw[2]+8192;loaded.set_value();go.wait();uint32_t v=7;std::memcpy(saved,&v,4);});
 ready.wait();table.phase(1,true,false,true);assert(!writes[2]&&!writes[4]);
 hash();assert(table.snapshot().phases[1].grace_pending==1);
 release.set_value();old.join();
 hash();assert(table.snapshot().phases[1].cpu_candidates==0);
 hash();assert(table.snapshot().phases[1].epoch_candidates==1);
 {epoch::RunLease newer(&d);hash();assert(table.snapshot().phases[1].epoch_candidates==2);}
 // A tracked same-value store is an event even when the content Hash is equal.
 uint32_t v=7;auto before=table.snapshot().phases[1].cpu_candidates;
 assert(table.write(raw.get(),16384,&v,4)==watch::Access::Complete);hash();
 assert(table.snapshot().phases[1].cpu_candidates==before);hash();
 // A coarse HLE interval does not explain an exact writer, and cannot qualify.
 before=table.snapshot().phases[1].epoch_candidates;
 {epoch::HostLease host(&d);v=8;std::memcpy(data,&v,4);hash();}
 hash();assert(table.snapshot().phases[1].epoch_candidates==before);hash();
 assert(table.snapshot().phases[1].epoch_candidates==before+1);
 // External activity DURING a baseline cannot be hidden at the next check.
 auto t=table.before(raw.get(),8192,data,4);
 {epoch::HostLease host(&d);v=9;std::memcpy(data,&v,4);}
 table.after(t,v);before=table.snapshot().phases[1].epoch_candidates;hash();
 assert(table.snapshot().phases[1].epoch_candidates==before);hash();
 d.gpu_boundary();before=table.snapshot().phases[1].epoch_candidates;hash();
 assert(table.snapshot().phases[1].epoch_candidates==before);hash();
 // Deliberately untracked write: independent reference MUST expose a mismatch.
 v=11;std::memcpy(data,&v,4);hash();assert(table.snapshot().phases[1].epoch_candidate_changes==1);
 for(unsigned i=0;i<40;++i){++v;std::memcpy(data,&v,4);hash();}
 assert(table.snapshot().epoch_mismatches_stored==32&&table.snapshot().epoch_mismatches_dropped==9);
 // Remaps discard in-flight hashes; identical PTE publication retains guards.
 t=table.before(raw.get(),8192,data,4);table.publish(raw.get(),2,raw[2]);assert(!writes[2]);
 table.publish(raw.get(),2,nullptr);assert(table.after(t,v).reason==watch::HashReason::Raced);
 table.phase(watch::no_index,false);table.detach(raw.get());std::free(data);
 assert(d.snapshot().entries==d.snapshot().exits&&!d.snapshot().active);
 // Saturation/reset fail closed without blocking or stealing a live lease.
 epoch::Domain full;std::vector<unsigned> slots;
 for(unsigned i=0;i<64;++i)slots.push_back(full.enter());
 assert(full.enter()==epoch::Domain::invalid_slot);assert(!full.stamp(full.publish()).drained);
 assert(!full.reset());for(auto i:slots)full.leave(i);assert(full.reset());
 // Concurrent metadata transitions: no store locks, waits or callbacks under domain lock.
 epoch::Domain concurrent;std::vector<std::thread> workers;
 for(unsigned k=0;k<4;++k)workers.emplace_back([&]{for(unsigned i=0;i<4096;++i){epoch::RunLease l(&concurrent);epoch::HostLease h(&concurrent);}});
 for(unsigned i=0;i<4096;++i)(void)concurrent.stamp(concurrent.publish());
 for(auto &w:workers) { w.join(); }
 auto s=concurrent.snapshot();
 assert(s.entries==16384&&s.exits==16384&&s.host_entries==16384&&s.host_exits==16384&&!s.failed);
 assert(concurrent.stamp(concurrent.publish()).drained);
}
'''
CONTROLLER = r'''
#include <util/switch_memory_validation.h>
#include <util/switch_write_watch_api.h>
#include <util/switch_capture_plan.h>
#include <util/switch_direct_heap.h>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <cstring>
#include <cstdlib>
#include <memory>
namespace sv=switch_validation;
int main(int argc,char **argv){
 assert(argc==2);std::filesystem::current_path(argv[1]);std::filesystem::create_directories("sdmc:/switch/vita3k");
 // Previous shipped heap_fault preset must follow the new compiled default.
 std::ofstream("sdmc:/switch/vita3k/tracelab.cfg")<<"mode=auto\nvalidation_experiment=heap_fault\n";
 sv::direct_heap::state.active.store(true);sv::direct_heap::state.pool_bytes.store(768ull<<20);
 assert(sv::deferred_import::registry.configure(true));
 sv::begin_session("EPOCH_FIXTURE");assert(sv::epoch::enabled.load());
 for(auto name:sv::compact_controls)sv::record("write_watch_canary",name,"pass","HOST controller fixture, not native evidence");
 auto raw=std::make_unique<uint8_t*[]>(1<<20);auto *data=static_cast<uint8_t*>(std::aligned_alloc(4096,4096));
 assert(data);std::memset(data,0,4096);sv::live_publish_page(raw.get(),2,reinterpret_cast<uint8_t*>(uintptr_t(data)-8192));
 auto **writes=sv::live_write_table(raw.get());assert(writes);
 uint64_t tick=10000;sv::on_render_end(tick);auto view=sv::status_snapshot();
 assert(view.stage==sv::Stage::Waiting&&view.total_ms==28000&&view.epoch_audit&&!view.heap_fault);
 assert(std::strstr(sv::make_hud_text(view,tick).title.data(),"LIVE EPOCH"));
 assert(!sv::strict_hash());sv::request_benchmark();sv::on_render_end(tick);
 for(unsigned phase=0;phase<4;++phase){
  assert(sv::status_snapshot().phase_count==4);
  assert(sv::status_snapshot().policy_letter==((phase==1||phase==2)?'B':'A'));
  for(unsigned n=0;n<70;++n){
   assert(sv::strict_hash());
   if(n%10==0){sv::epoch::HostLease host;sv::epoch::gpu_boundary();}
   if((phase==1||phase==2)&&n==20){
    sv::epoch::RunLease jit;uint32_t v=phase*100+n;
    assert(!writes[2]);assert(sv::live_write(raw.get(),8192,&v,4)==sv::watch::Access::Complete);
   }
   if(phase==1&&n==31){uint32_t v=333;std::memcpy(data,&v,4);} // deliberate missing writer
   {sv::epoch::RunLease jit;}
   auto token=sv::live_hash_begin(raw.get(),8192,data,4,nullptr,0);
   uint32_t value;std::memcpy(&value,data,4);sv::live_hash_end(token,value);
   tick+=100;sv::on_render_end(tick);
  }
 }
 view=sv::status_snapshot();assert(view.stage==sv::Stage::Complete&&view.outcome==sv::CaptureOutcome::Ready);
 assert(view.epoch_changes==1&&view.grace_ready&&view.epoch_candidates&&!sv::strict_hash());
 assert(writes[2]);sv::frontend_poll(true);assert(sv::status_snapshot().stage==sv::Stage::Complete);
 std::filesystem::create_directory("report");sv::write_report("report");
 sv::live_detach_table(raw.get());sv::end_session();assert(!sv::epoch::enabled.load());std::free(data);
 std::ofstream("sdmc:/switch/vita3k/tracelab.cfg")<<"mode=off\n";
 sv::begin_session("OFF");assert(!sv::enabled()&&!sv::epoch::enabled.load());sv::end_session();
 // Explicit legacy debugging is still possible without changing the default build.
 std::ofstream("sdmc:/switch/vita3k/tracelab.cfg")<<"validation_experiment=legacy_heap_fault\n";
 sv::begin_session("LEGACY");assert(!sv::epoch::enabled.load());sv::end_session();
}
'''
def load(path,name):
    spec=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m

def source_gate(root,required):
    expected={
        'vita3k/cpu/src/dynarmic_cpu.cpp':('epoch::RunLease publication_reader;', 'halt_reason = jit->Run();', 'jit->Step();'),
        'vita3k/kernel/src/thread.cpp':('epoch::HostLease host_activity;', 'kernel.call_import(*cpu, nid, id);'),
        'vita3k/renderer/src/vulkan/renderer.cpp':('epoch::gpu_boundary();',),
        'vita3k/util/src/switch_memory_validation.cpp':('VITA3K_SWITCH_EPOCH_AUDIT_DEFAULT','epoch::domain.reset()','epoch_audit'),
    }
    for path,needles in expected.items():
        p=root/path
        if not p.exists():
            if required:raise RuntimeError('missing actual integration: '+path)
            print('SKIP unavailable actual integration:',path);continue
        text=p.read_text()
        if not all(n in text for n in needles):raise RuntimeError('unapplied integration: '+path)
        if path.endswith('renderer.cpp') and text.count('epoch::gpu_boundary();')!=4:
            raise RuntimeError('All four general submission gateways must be covered')
        if path.endswith('dynarmic_cpu.cpp') and text.count('epoch::RunLease publication_reader;')!=2:
            raise RuntimeError('Run AND Step must be leased')

def main():
    p=argparse.ArgumentParser();p.add_argument('--cxx',default='c++');p.add_argument('--sanitize',action='store_true');p.add_argument('--require-source',action='store_true');a=p.parse_args()
    source_gate(ROOT,a.require_source)
    with tempfile.TemporaryDirectory() as tmp:
        d=Path(tmp);inc=d/'include';(inc/'util').mkdir(parents=True);(inc/'switch/arm').mkdir(parents=True)
        (inc/'util/log.h').write_text('#pragma once\n#define LOG_INFO(...) ((void)0)\n#define LOG_ERROR(...) ((void)0)\n#define LOG_WARN(...) ((void)0)\n')
        (inc/'switch/arm/counter.h').write_text('#pragma once\n#include <cstdint>\ninline uint64_t armGetSystemTickFreq(){return 1000;}\n')
        common=[a.cxx,'-std=c++20','-O2','-Wall','-Wextra','-Werror','-pthread','-I',str(inc),'-I',str(ROOT/'vita3k/util/include')]
        if a.sanitize:common+=['-fsanitize=address,undefined','-fno-omit-frame-pointer']
        (d/'pure.cpp').write_text(PURE);subprocess.run(common+[str(d/'pure.cpp'),'-o',str(d/'pure')],check=True)
        subprocess.run([str(d/'pure')],check=True,timeout=30)
        (d/'controller.cpp').write_text(CONTROLLER)
        subprocess.run(common+['-D__SWITCH__','-DVITA3K_SWITCH_TRACELAB=1','-DVITA3K_SWITCH_CLEAN_CAPTURE_DEFAULT=1',
            '-DVITA3K_SWITCH_HEAP_FAULT_DEFAULT=1','-DVITA3K_SWITCH_EPOCH_AUDIT_DEFAULT=1',str(d/'controller.cpp'),
            str(ROOT/'vita3k/util/src/switch_memory_validation.cpp'),'-o',str(d/'controller')],check=True)
        subprocess.run([str(d/'controller'),str(d)],check=True,timeout=30)
        data=json.loads((d/'report/memory-validation.json').read_text())
        analyzer=load(ROOT/'scripts/analyze-switch-epoch.py','epoch_analysis');out=analyzer.analyze(data)
        assert not out['exclusions'],out['exclusions'];assert out['totals']['epoch_candidate_changes']==1
        assert len(data['epoch_audit']['mismatches'])==1
        assert not out['safe_to_skip_hash'] and out['comparison'] is not None
        for key,value in [('failed',True),('GPU_completion_covered',True),('active_jits_at_export',1),('jit_entries',0)]:
            bad=copy.deepcopy(data);bad['epoch_audit'][key]=value;assert analyzer.analyze(bad)['exclusions']
        for i in range(4):
            bad=copy.deepcopy(data);bad['epoch_audit']['phases'][i]['index']=99;assert analyzer.analyze(bad)['exclusions']
        for action in ('missing_mismatch','wrong_ticket','bad_backing','bad_cache_ledger'):
            bad=copy.deepcopy(data)
            if action=='missing_mismatch':bad['epoch_audit']['mismatches']=[]
            elif action=='wrong_ticket':bad['epoch_audit']['mismatches'][0]['publication']=0
            elif action=='bad_backing':bad['guest_backing']['active']=False
            else:bad['write_shadow']['cache_ledger']['changes']+=1
            assert analyzer.analyze(bad)['exclusions'],action
        for action in ('remove','duplicate','fail'):
            bad=copy.deepcopy(data)
            if action=='remove':bad['tests'].pop(next(i for i,t in enumerate(bad['tests']) if t['suite']=='write_watch_canary'))
            elif action=='duplicate':bad['tests'].append(next(t for t in bad['tests'] if t['suite']=='write_watch_canary'))
            else:next(t for t in bad['tests'] if t['suite']=='write_watch_canary')['status']='fail'
            assert analyzer.analyze(bad)['exclusions']
    print('Epoch audit PASS: stale-loaded PTE exclusion, post-publication active readers, exact/same-value/alias writes, HLE/GPU epochs, missing-writer negative, saturation, 16384 concurrent leases, actual controller/HUD/JSON/analyzer; no native/GPU execution claim')
if __name__=='__main__':main()
