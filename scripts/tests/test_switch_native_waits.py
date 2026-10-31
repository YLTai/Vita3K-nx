#!/usr/bin/env python3
"""Real metadata/Queue/schedule tests. No native A64 or GPU-execution claim."""
import argparse
import copy
import os
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[2]
CPP = r'''
#include <util/jit_epoch_gate.h>
#include <util/sampled_phase_clock.h>
#include <threads/queue.h>
#include <cassert>
#include <atomic>
#include <chrono>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
using namespace switch_validation;
static uint64_t ticket;
static thread_local bool check_copy = false;
struct Item {
    Item() = default;
    Item(const Item &) { if(check_copy) assert(epoch::domain.stamp(ticket,0).ranges.unknown_busy == 1); }
    Item &operator=(const Item &) = default;
};
template<class Predicate> void until(Predicate p) {
    const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while(!p()) { assert(std::chrono::steady_clock::now()<end); std::this_thread::yield(); }
}
void waits() {
    epoch::Domain d; alignas(4096) uint8_t bytes[8192]{};
    d.watch_range(0,bytes,32,nullptr,0);const auto t=d.publish();
    const auto before=d.stamp(t,0).ranges;
    {
        epoch::HostLease h(&d,1);epoch::name_import("unreviewed_writer_then_wait");
        assert(d.stamp(t,0).ranges.unknown_busy==1);
        { epoch::NativeWaitLease parked;
          const auto a=d.stamp(t,0).ranges;
          assert(!a.unknown_busy && a.unknown!=before.unknown);
          {epoch::NativeWaitLease nested;assert(!d.stamp(t,0).ranges.unknown_busy);}
          assert(!d.stamp(t,0).ranges.unknown_busy);
          assert(!epoch::declare_outputs({"unreviewed_writer_then_wait"},{}));
          {epoch::HostLease other(&d,2);epoch::name_import("other_writer");
           assert(d.stamp(t,0).ranges.unknown_busy==1);}
          // The other writer's unknown history must survive its completion.
          assert(!epoch::stable_host(a,d.stamp(t,0).ranges));
        }
        assert(d.stamp(t,0).ranges.unknown_busy==1);
        try {epoch::NativeWaitLease unwinding;throw std::runtime_error("wait failure");}
        catch(const std::runtime_error &) {}
        assert(d.stamp(t,0).ranges.unknown_busy==1);
    }
    assert(!d.stamp(t,0).ranges.unknown_busy&&!d.snapshot().failed);
    // Scoped output spans MUST remain busy while the caller sleeps; another
    // thread may fill these outputs. Neither same-page nor wakeup clears them.
    {epoch::HostLease output(&d,3);epoch::name_import("event_wait");
     assert(epoch::declare_outputs({"event_wait"},{{bytes,4}}));
     {epoch::NativeWaitLease guard;assert(d.stamp(t,0).ranges.busy==1);}
     assert(d.stamp(t,0).ranges.busy==1);}
    assert(!d.stamp(t,0).ranges.busy);
    {epoch::HostLease output(&d,4);epoch::name_import("disjoint_wait");
     assert(epoch::declare_outputs({"disjoint_wait"},{{bytes+128,4}}));
     const auto a=d.stamp(t,0).ranges;epoch::NativeWaitLease guard;
     assert(epoch::stable_host(a,d.stamp(t,0).ranges));}
    // Real concurrent metadata wait guards; none span the Domain lock.
    std::vector<std::thread> workers;
    for(unsigned k=0;k<4;++k)workers.emplace_back([&,k]{for(unsigned j=0;j<1024;++j){
        epoch::HostLease h(&d,100+k);epoch::name_import("stress");
        {epoch::NativeWaitLease w;epoch::NativeWaitLease nested;}
    }});
    for(auto &w:workers) { w.join(); }
    assert(!d.snapshot().failed&&d.snapshot().host_active==0);
    FILE *f=std::fopen("native-waits.json","wb");assert(f);d.write_ranges(f);std::fclose(f);
    // The actual queue must restore writer status BEFORE copying a queued item.
    auto &qdomain=epoch::domain;assert(qdomain.reset());qdomain.watch_range(0,bytes,32,nullptr,0);
    ticket=qdomain.publish();epoch::enabled.store(true);
    Queue<Item> q;q.maxPendingCount_=1;Item item;q.push(item);
    std::atomic<bool> entered=false,done=false;
    std::thread producer([&]{epoch::HostLease h;epoch::name_import("queue_producer");check_copy=true;entered=true;q.push(item);check_copy=false;done=true;});
    until([&]{return entered&&qdomain.snapshot().host_active==1&&!qdomain.stamp(ticket,0).ranges.unknown_busy;});
    assert(!done);assert(q.pop());producer.join();assert(done);
    std::atomic<bool> waiting=false;
    std::thread finisher([&]{epoch::HostLease h;epoch::name_import("queue_finish");waiting=true;q.wait_empty();});
    until([&]{return waiting&&qdomain.snapshot().host_active==1&&!qdomain.stamp(ticket,0).ranges.unknown_busy;});
    assert(q.pop());finisher.join();assert(!qdomain.snapshot().failed&&qdomain.snapshot().host_active==0);
    epoch::enabled.store(false);{epoch::NativeWaitLease off;}
}
void schedules() {
    for(uint64_t frame:{16ull,100ull,130ull,200ull,1000ull}) {
        SampledPhaseClock c;c.reset(1000,1000);uint64_t now=1000;unsigned ended=0;uint64_t n=0,duration=0;
        while(!c.complete()) {
            now+=frame;const auto s=c.advance(now);
            if(s.sample){++n;duration+=s.delta;}
            if(s.ended){
                assert(s.phase==ended++);
                if(!s.capped){assert(n>=50&&duration>=6000&&duration<=12000);}
                else assert(frame==1000);
                n=duration=0;
            }
        }
        assert(ended==4&&!c.failed()&&now<=1000+4*(13000+frame));
    }
    SampledPhaseClock stall;stall.reset(1000,1000);
    auto s=stall.advance(30000);assert(s.ended&&s.capped&&!s.sample&&!s.complete&&stall.index()==1);
    // A long stall cannot silently complete all future A/B phases.
    assert(!stall.advance(30000).sample&&stall.index()==1);
    stall.defer_start(31000);assert(!stall.advance(31500).sample);
    assert(stall.advance(32000).sample==false);
    assert(stall.advance(32100).sample);
    SampledPhaseClock restart;restart.reset(5000,1000);assert(!restart.advance(4999).sample&&restart.failed());
    restart.reset(6000,1000);assert(!restart.failed()&&restart.index()==0&&restart.samples()==0);
    SampledPhaseClock bad;bad.reset(0,0);assert(bad.failed());
}
int main(){waits();schedules();}
'''

def source_gate(required):
    recipe=json.loads((ROOT/'patches/experiments/switch-native-waits.json').read_text())
    if not required:return
    for f in recipe['files']:
        text=(ROOT/f['path']).read_text()
        for op in f['replacements']:
            assert op['new'] in text, f['path']+': missing exact native-wait/sample hook'
    # Scope ends before output/notification/callback execution. No broad named
    # no-write allow-list was used for audio or GXM DisplayQueueAddEntry.
    queue=(ROOT/'vita3k/threads/include/threads/queue.h').read_text()
    assert 'The wait lease is gone before queue_.push' in queue
    display=(ROOT/'vita3k/display/src/display.cpp').read_text()
    assert display.index('Resume the unknown prefix BEFORE')<display.index('    if (is_cb) {')

def load(path, name):
    spec=importlib.util.spec_from_file_location(name,path)
    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module);return module

def controller_gate(d, cxx, sanitize):
    fixture=load(ROOT/'scripts/tests/test_switch_epoch_audit.py','wait_epoch_fixture')
    inc=d/'include';(inc/'util').mkdir(parents=True);(inc/'switch/arm').mkdir(parents=True)
    (inc/'util/log.h').write_text('#pragma once\n#define LOG_INFO(...) ((void)0)\n#define LOG_ERROR(...) ((void)0)\n#define LOG_WARN(...) ((void)0)\n')
    (inc/'switch/arm/counter.h').write_text('#pragma once\n#include <cstdint>\ninline uint64_t armGetSystemTickFreq(){return 1000;}\n')
    code=fixture.CONTROLLER.replace('uint64_t tick=10000;',
        'const uint64_t frame=std::strtoull(std::getenv("FIXTURE_FRAME_MS"),nullptr,10); uint64_t tick=10000;')
    code=code.replace('for(unsigned phase=0;phase<4;++phase){',
        'for(unsigned phase=0;phase<4;++phase){\n sv::epoch::HostLease host; sv::epoch::name_import("unknown_native_wait"); sv::epoch::NativeWaitLease parked;')
    code=code.replace('for(unsigned n=0;n<70;++n){',
        'for(unsigned n=0;sv::status_snapshot().stage!=sv::Stage::Complete && sv::status_snapshot().phase==phase+1;++n){ assert(n<200);')
    code=code.replace('tick+=100;', 'tick+=frame;')
    code=code.replace('view.outcome==sv::CaptureOutcome::Ready',
        'view.outcome==(frame>=1000?sv::CaptureOutcome::Invalid:sv::CaptureOutcome::Ready)')
    code=code.replace('assert(view.epoch_changes==1&&view.grace_ready&&view.epoch_candidates&&!sv::strict_hash());',
        'assert(!sv::strict_hash()); if(frame<1000)assert(view.hle_changes==1&&view.grace_ready&&view.hle_candidates);')
    (d/'controller.cpp').write_text(code)
    command=[cxx,'-std=c++20','-O2','-Wall','-Wextra','-Werror','-pthread',
        '-I',str(inc),'-I',str(ROOT/'vita3k/util/include'),'-D__SWITCH__','-DVITA3K_SWITCH_TRACELAB=1',
        '-DVITA3K_SWITCH_CLEAN_CAPTURE_DEFAULT=1','-DVITA3K_SWITCH_HEAP_FAULT_DEFAULT=1','-DVITA3K_SWITCH_EPOCH_AUDIT_DEFAULT=1',
        str(d/'controller.cpp'),str(ROOT/'vita3k/util/src/switch_memory_validation.cpp'),'-o',str(d/'controller')]
    if sanitize:command+=['-fsanitize=address,undefined','-fno-omit-frame-pointer']
    subprocess.run(command,check=True)
    analyzer=load(ROOT/'scripts/analyze-switch-epoch.py','wait_analyzer')
    for frame in (100,130,1000):
        run=d/f'frame-{frame}';run.mkdir()
        subprocess.run([str(d/'controller'),str(run)],env=dict(os.environ,FIXTURE_FRAME_MS=str(frame)),check=True,timeout=30)
        data=json.loads((run/'report/memory-validation.json').read_text());out=analyzer.analyze(data)
        assert out['evidence_eligible'],out['exclusions']
        assert data['sample_policy']['minimum_samples']==50 and not out['safe_to_skip_hash']
        native=data['epoch_audit']['range_contracts']['native_waits']
        assert native['parks']==native['resumes']==4 and native['active']==0
        if frame<1000:
            assert not out['exclusions'] and out['comparison_eligible'],out['exclusions']
            assert out['scoped_hle']['totals']['hle_candidate_changes']==1
            assert all(p['measured_intervals']>=50 and p['duration_ms']>=6000 for p in data['benchmark']['phases'])
        else:
            assert out['performance_exclusions'] and not out['comparison_eligible'] and out['comparison'] is None
            assert all(data['sample_policy']['capped_phases'])
        for key,value in [('parks',-1),('resumes',native['parks']+1),('active',1),('async_outputs_covered',True)]:
            bad=copy.deepcopy(data);bad['epoch_audit']['range_contracts']['native_waits'][key]=value
            assert not analyzer.analyze(bad)['evidence_eligible'],key
        for key,value in [('version',2),('minimum_samples',1),('maximum_seconds',100),('capped_phases',[False])]:
            bad=copy.deepcopy(data);bad['sample_policy'][key]=value
            assert not analyzer.analyze(bad)['comparison_eligible'],key
        if frame==130:
            # Old captures without the explicit new policy keep their old gate.
            legacy=copy.deepcopy(data);legacy.pop('sample_policy')
            assert not analyzer.analyze(legacy)['comparison_eligible']
    print('Actual controller/HUD/export/analyzer PASS: 100/130/1000ms frames, adaptive extension/cap, parked-root positive candidates, deliberate counterexample, wait ledger negatives, legacy gates unchanged; synthetic frame timing only')

def main():
    p=argparse.ArgumentParser();p.add_argument('--cxx',default='c++');p.add_argument('--sanitize',action='store_true');p.add_argument('--require-source',action='store_true');a=p.parse_args()
    source_gate(a.require_source)
    with tempfile.TemporaryDirectory() as tmp:
        d=Path(tmp);(d/'test.cpp').write_text(CPP)
        cmd=[a.cxx,'-std=c++20','-O2','-Wall','-Wextra','-Werror','-pthread','-D__SWITCH__','-DVITA3K_SWITCH_TRACELAB=1',
             '-I',str(ROOT/'vita3k/util/include'),'-I',str(ROOT/'vita3k/threads/include'),str(d/'test.cpp'),'-o',str(d/'test')]
        if a.sanitize:cmd+=['-fsanitize=address,undefined','-fno-omit-frame-pointer']
        subprocess.run(cmd,check=True);subprocess.run([str(d/'test')],cwd=d,check=True,timeout=30)
        data=json.loads((d/'native-waits.json').read_text());n=data['native_waits']
        assert n['version']==1 and n['parks']==n['resumes'] and n['parks']>=4098 and n['active']==0
        assert data['calls']==data['exits'] and data['failed'] is False
        assert sum(r['parks'] for r in data['imports'])==n['parks']
        assert n['async_outputs_covered'] is False
        controller_gate(d,a.cxx,a.sanitize)
    print('Native waits and sample clock PASS: prefix/suffix, nested/throw/other-writer, busy output retention, 4096 concurrent leases, actual Queue resume-before-copy, 5 frame rates/stalls/caps/restart; host execution only')
if __name__=='__main__':main()
