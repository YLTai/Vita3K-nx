#!/usr/bin/env python3
"""Exact HLE-range evidence; host stores are models, never device execution.
Full checkout --require-source also verifies the exact production recipe anchors.
"""
import argparse
import copy
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
CPP = r'''
#include <util/switch_write_watch.h>
#include <cassert>
#include <cstdlib>
#include <future>
#include <thread>
#include <vector>
using namespace switch_validation;
int main() {
 epoch::Domain d; watch::Table table; table.set_epoch_domain(&d);
 auto raw=std::make_unique<uint8_t*[]>(watch::page_count);
 auto *data=static_cast<uint8_t*>(std::aligned_alloc(4096,12288)); assert(data);
 std::memset(data,0,12288);
 raw[2]=reinterpret_cast<uint8_t*>(uintptr_t(data)-8192);
 raw[4]=reinterpret_cast<uint8_t*>(uintptr_t(data)-16384);
 auto **writes=table.bind(raw.get()); assert(writes);
 table.phase(0,false,true);
 auto hash=[&]{auto t=table.before(raw.get(),8192,data,4,data+64,4);uint32_t v,p;
  std::memcpy(&v,data,4);std::memcpy(&p,data+64,4);return table.after(t,v^p);};
 hash();table.phase(1,true,false,true);hash();hash();
 assert(table.snapshot().phases[1].hle_candidates==1);
 // A real long-lived import on an unrelated workarea no longer vetoes all textures.
 {
  epoch::HostLease wait(&d,0x46e7be7b);epoch::name_import("sceKernelLockLwMutex");
  assert(epoch::declare_outputs({"sceKernelLockLwMutex"},{{data+4096,32}}));
  auto n=table.snapshot().phases[1].hle_candidates;hash();hash();
  assert(table.snapshot().phases[1].hle_candidates==n+2);
  assert(!table.snapshot().phases[1].epoch_candidates || d.snapshot().host_active==1);
 }
 // Same page, disjoint bytes: no false overlap. Palette-only writes ARE overlaps.
 {epoch::HostLease h(&d,2);epoch::name_import("memset");
  assert(epoch::declare_outputs({"memset"},{{data+128,4}}));
  auto n=table.snapshot().phases[1].hle_candidates;std::memset(data+128,7,4);hash();
  assert(table.snapshot().phases[1].hle_candidates==n+1);}
 auto n=table.snapshot().phases[1].hle_candidates;
 {epoch::HostLease h(&d,3);epoch::name_import("memcpy");
  assert(epoch::declare_outputs({"memcpy"},{{data+64,4}}));std::memset(data+64,5,4);hash();}
 hash();assert(table.snapshot().phases[1].hle_candidates==n);hash();
 // Begin/end on either side of a Hash invalidates it; same-value writes count.
 auto t=table.before(raw.get(),8192,data,4,data+64,4);
 {epoch::HostLease h(&d,4);epoch::name_import("memset");
  assert(epoch::declare_outputs({"memset"},{{data,4}}));std::memset(data,0,4);}
 uint32_t p;std::memcpy(&p,data+64,4);table.after(t,p);
 n=table.snapshot().phases[1].hle_candidates;hash();assert(table.snapshot().phases[1].hle_candidates==n);hash();
 // Unknown imports stay globally excluding. A nested helper cannot narrow root.
 {epoch::HostLease root(&d,5);epoch::name_import("unknown_outer");epoch::name_import("memcpy");
  assert(!epoch::declare_outputs({"memcpy"},{{data+8192,4}}));
  n=table.snapshot().phases[1].hle_candidates;hash();assert(table.snapshot().phases[1].hle_candidates==n);
  {epoch::HostLease inner(&d,6);epoch::name_import("memcpy");assert(epoch::declare_outputs({"memcpy"},{{data+8192,4}}));hash();}
  assert(table.snapshot().phases[1].hle_candidates==n);}
 hash();hash();
 // A callback prefix can write anywhere before its reviewed wait tail.
 {epoch::HostLease cb(&d,7);epoch::name_import("sceKernelWaitLwCondCB");std::memset(data,3,4);
  assert(epoch::declare_outputs({"sceKernelWaitLwCondCB"},{{data+4096,32}},true));
  n=table.snapshot().phases[1].hle_candidates;hash();assert(table.snapshot().phases[1].hle_candidates==n);hash();}
 // GPU submission history stays an independent exclusion, not completion proof.
 n=table.snapshot().phases[1].hle_candidates;auto g=table.snapshot().phases[1].gpu_epoch_blocks;
 d.gpu_boundary();hash();assert(table.snapshot().phases[1].hle_candidates==n+1&&table.snapshot().phases[1].gpu_epoch_blocks==g+1);
 // Unrelated map changes do NOT restart publication grace.
 auto pubs=d.snapshot().publications;table.publish(raw.get(),8,reinterpret_cast<uint8_t*>(uintptr_t(data+8192)-32768));
 assert(d.snapshot().publications==pubs&&table.snapshot().phases[1].unrelated_mapping_updates==1);
 // Same backing aliases retain interception and invalidate affected inputs.
 uint32_t value=9;assert(!writes[4]);assert(table.write(raw.get(),16384,&value,4)==watch::Access::Complete);
 n=table.snapshot().phases[1].hle_candidates;hash();assert(table.snapshot().phases[1].hle_candidates==n);hash();
 // Intentionally untracked writes must create bounded counterexamples, never reuse.
 for(unsigned i=0;i<40;++i){++value;std::memcpy(data,&value,4);hash();}
 auto s=table.snapshot();assert(s.hle_mismatches_stored==32&&s.hle_mismatches_dropped==8);
 const auto &c=s.phases[1];assert(c.hle_candidates+c.hle_unknown_blocks+c.hle_range_blocks+c.hle_history_blocks==c.cpu_candidates);
 assert(c.submission_candidates+c.gpu_epoch_blocks==c.hle_candidates);
 // A resource first discovered during an output lease is busy from its first stamp.
 epoch::Domain fresh;auto ticket=fresh.publish();
 {epoch::HostLease h(&fresh,8);epoch::name_import("memcpy");assert(epoch::declare_outputs({"memcpy"},{{data,8}}));
  fresh.watch_range(0,data,4,nullptr,0);assert(fresh.stamp(ticket,0).ranges.busy==1);
  fresh.clear_interests();fresh.watch_range(0,data,4,nullptr,0);assert(fresh.stamp(ticket,0).ranges.busy==1);}
 assert(!fresh.stamp(ticket,0).ranges.busy&&!fresh.snapshot().failed);
 // Exhaustion and invalid range declarations never create absence authority.
 epoch::Domain full;std::vector<std::unique_ptr<epoch::HostLease>> leases;
 for(unsigned i=0;i<129;++i)leases.emplace_back(std::make_unique<epoch::HostLease>(&full,i));
 assert(full.snapshot().failed);while(!leases.empty())leases.pop_back();assert(full.reset());
 {epoch::HostLease h(&fresh,9);epoch::name_import("memcpy");
  assert(!epoch::declare_outputs({"memcpy"},{{reinterpret_cast<void*>(UINTPTR_MAX-1),8}}));
  assert(fresh.stamp(ticket,0).ranges.unknown_busy==1);}
 // Metadata race stress: disjoint scoped writers stay unrelated; unknowns remain barriers.
 epoch::Domain concurrent;concurrent.watch_range(0,data,4,nullptr,0);auto tk=concurrent.publish();
 std::vector<std::thread> threads;
 for(unsigned k=0;k<4;++k)threads.emplace_back([&,k]{for(unsigned i=0;i<4096;++i){
   epoch::RunLease r(&concurrent);epoch::HostLease h(&concurrent,k+1);epoch::name_import("memset");
   assert(epoch::declare_outputs({"memset"},{{data+4096+k*32,4}}));}});
 for(unsigned i=0;i<4096;++i)(void)concurrent.stamp(tk,0);
 for(auto &w:threads){w.join();}auto cs=concurrent.snapshot();assert(!cs.failed&&cs.host_entries==16384&&cs.host_exits==16384);
 assert(!concurrent.stamp(tk,0).ranges.busy);
 table.phase(watch::no_index,false);table.detach(raw.get());std::free(data);
}
'''

def load(path, name):
    spec=importlib.util.spec_from_file_location(name,path)
    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module);return module

def source_gate(required):
    recipe=json.loads((ROOT/'patches/experiments/switch-scoped-hle.json').read_text())
    for f in recipe['files']:
        p=ROOT/f['path']
        if not p.exists():
            if required:raise RuntimeError('missing production source: '+f['path'])
            continue
        text=p.read_text()
        # Some later replacements extend earlier inserted blocks. Check the
        # final declaration markers and exact one-site insertions by extraction.
        for op in f['replacements']:
            needle=op['new']
            if needle not in text and not any(needle in newer['old'] for newer in f['replacements']):
                if required:raise RuntimeError('unapplied production contract: '+f['path']+' '+needle[:90])
    # Review boundaries are pinned by the engine. Callbacks must precede the
    # declared wait tail, not run while a narrower scope claims completeness.
    thread=ROOT/'vita3k/modules/SceKernelThreadMgr/SceThreadmgr.cpp'
    if thread.exists():
        text=thread.read_text()
        for name in ('_sceKernelWaitLwCondCB','_sceKernelWaitCondCB','_sceKernelWaitSemaCB'):
            begin=text.index(', '+name+',');end=text.index('\n}',begin);body=text[begin:end]
            assert body.index('process_callbacks(')<body.index('return '),name
    if required:
        assert 'nid);' in (ROOT/'vita3k/kernel/src/thread.cpp').read_text()
        assert 'name_import(export_name);' in (ROOT/'vita3k/module/include/module/bridge.h').read_text()

def controller_gate(d, cxx, sanitize):
    old=load(ROOT/'scripts/tests/test_switch_epoch_audit.py','scoped_epoch_fixture')
    inc=d/'include';(inc/'util').mkdir(parents=True);(inc/'switch/arm').mkdir(parents=True)
    (inc/'util/log.h').write_text('#pragma once\n#define LOG_INFO(...) ((void)0)\n#define LOG_WARN(...) ((void)0)\n#define LOG_ERROR(...) ((void)0)\n')
    (inc/'switch/arm/counter.h').write_text('#pragma once\n#include <cstdint>\ninline uint64_t armGetSystemTickFreq(){return 1000;}\n')
    code=old.CONTROLLER.replace('for(unsigned phase=0;phase<4;++phase){',
        'for(unsigned phase=0;phase<4;++phase){\n sv::epoch::HostLease wait; sv::epoch::name_import("reviewed_wait");\n assert(sv::epoch::declare_outputs({"reviewed_wait"},{{data+128,16}}));')
    # Existing coarse candidates are intentionally excluded by the long wait;
    # the new range contract can still produce positive and negative evidence.
    code=code.replace('view.epoch_changes==1&&view.grace_ready&&view.epoch_candidates',
                      'view.hle_changes==1&&view.grace_ready&&view.hle_candidates')
    (d/'controller.cpp').write_text(code)
    cmd=[cxx,'-std=c++20','-O2','-Wall','-Wextra','-Werror','-pthread',
         '-I',str(inc),'-I',str(ROOT/'vita3k/util/include'),'-D__SWITCH__','-DVITA3K_SWITCH_TRACELAB=1',
         '-DVITA3K_SWITCH_CLEAN_CAPTURE_DEFAULT=1','-DVITA3K_SWITCH_HEAP_FAULT_DEFAULT=1','-DVITA3K_SWITCH_EPOCH_AUDIT_DEFAULT=1',
         str(d/'controller.cpp'),str(ROOT/'vita3k/util/src/switch_memory_validation.cpp'),'-o',str(d/'controller')]
    if sanitize:cmd+=['-fsanitize=address,undefined','-fno-omit-frame-pointer']
    subprocess.run(cmd,check=True);subprocess.run([str(d/'controller'),str(d)],check=True,timeout=30)
    data=json.loads((d/'report/memory-validation.json').read_text())
    analyzer=load(ROOT/'scripts/analyze-switch-epoch.py','scoped_analyzer');out=analyzer.analyze(data)
    assert not out['exclusions'],out['exclusions']
    totals=out['scoped_hle']['totals'];assert totals['hle_candidates'] and totals['hle_candidate_changes']==1
    assert not out['totals']['epoch_candidates'] and not out['safe_to_skip_hash']
    assert out['scoped_hle']['contracts']['scoped_calls']==4
    for key,value in [('authority',True),('failed',True),('calls',0),('exits',0),('version',0),('attribution_period',1)]:
        bad=copy.deepcopy(data);bad['epoch_audit']['range_contracts'][key]=value
        assert analyzer.analyze(bad)['exclusions'],key
    for key in ('hle_candidates','hle_unknown_blocks','hle_range_blocks','hle_history_blocks','submission_candidates','gpu_epoch_blocks'):
        bad=copy.deepcopy(data);bad['epoch_audit']['range_phases'][1][key]+=1
        assert analyzer.analyze(bad)['exclusions'],key
    for key in ('range_mismatches','range_phases'):
        bad=copy.deepcopy(data);bad['epoch_audit'][key]=[];assert analyzer.analyze(bad)['exclusions'],key
    bad=copy.deepcopy(data);bad['epoch_audit']['range_contracts']['imports']*=2
    assert analyzer.analyze(bad)['exclusions']
    print('Actual controller/HUD/JSON/analyzer PASS: a long unrelated HLE permits scoped candidates, coarse veto stays, deliberate missing writer retained; corrupt/unsupported reports rejected (synthetic frame fixture)')

def main():
    p=argparse.ArgumentParser();p.add_argument('--cxx',default='c++');p.add_argument('--sanitize',action='store_true')
    p.add_argument('--require-source',action='store_true');a=p.parse_args();source_gate(a.require_source)
    with tempfile.TemporaryDirectory() as tmp:
        d=Path(tmp);(d/'test.cpp').write_text(CPP)
        cmd=[a.cxx,'-std=c++20','-O2','-Wall','-Wextra','-Werror','-pthread','-I',str(ROOT/'vita3k/util/include'),str(d/'test.cpp'),'-o',str(d/'test')]
        if a.sanitize:cmd+=['-fsanitize=address,undefined','-fno-omit-frame-pointer']
        subprocess.run(cmd,check=True);subprocess.run([str(d/'test')],check=True,timeout=45)
        controller_gate(d,a.cxx,a.sanitize)
    print('Scoped HLE PASS: disjoint/overlap/palette/same-value/alias, unknown caller and callback-prefix retention, both Hash endpoints, mapping grace, counterexamples, resource discovery/reset, saturation and 16384 concurrent leases; host model only')
if __name__=='__main__':main()
