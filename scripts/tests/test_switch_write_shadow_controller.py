#!/usr/bin/env python3
"""Run the actual v3 controller, runtime APIs, HUD and analyzer together."""
import argparse
import copy
import importlib.util
import json
import subprocess
import tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]

def main():
    p=argparse.ArgumentParser();p.add_argument('--cxx',default='c++');a=p.parse_args()
    with tempfile.TemporaryDirectory() as d:
        tmp=Path(d); inc=tmp/'include';(inc/'switch/arm').mkdir(parents=True);(inc/'util').mkdir()
        (inc/'switch/arm/counter.h').write_text('#pragma once\n#include <cstdint>\ninline uint64_t armGetSystemTickFreq(){return 1000;}\n')
        (inc/'util/log.h').write_text('#pragma once\n#define LOG_INFO(...) ((void)0)\n#define LOG_WARN(...) ((void)0)\n#define LOG_ERROR(...) ((void)0)\n')
        (tmp/'main.cpp').write_text(r'''
#include <util/switch_memory_validation.h>
#include <util/switch_write_watch_api.h>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
namespace sv=switch_validation;
uint64_t tick=10000;
uint32_t h1=1,h2=2;
void hash(uint8_t **pt,uint8_t *bytes){
    static uint32_t old1=h1, old2=h2;
    auto a=sv::live_hash_begin(pt,0x20000,bytes,64,nullptr,0);sv::live_hash_end(a,h1);
    sv::live_cache_decision(0,0x20000,64,old1^1,h1^1,true); old1=h1;
    auto b=sv::live_hash_begin(pt,0x21000,bytes+4096,64,nullptr,0);sv::live_hash_end(b,h2);
    sv::live_cache_decision(1,0x21000,64,old2^1,h2^1,true); old2=h2;
}
int main(int argc,char**argv){
 assert(argc==2);std::filesystem::current_path(argv[1]);
 std::filesystem::create_directories("sdmc:/switch/vita3k");
 {std::ofstream f("sdmc:/switch/vita3k/tracelab.cfg");f<<"mode=auto\nmemory_validation=on\nvalidation_experiment=write_shadow\nvalidation_phase_seconds=5\nvalidation_settle_seconds=1\n";}
 auto pt=std::make_unique<uint8_t*[]>(1<<20);
 std::unique_ptr<uint8_t,decltype(&std::free)> bytes(static_cast<uint8_t*>(std::aligned_alloc(4096,8192)),&std::free);
 assert(bytes);std::memset(bytes.get(),0,8192);
 sv::begin_session("LIVE");
 // Synthetic startup transcript for ANALYZER contract tests, NOT native execution.
 for (const char *name : {"native_exact_store", "native_repeat_alias_store", "raw_after_cpu_negative_control",
     "page_neighbor_negative_control", "host_notice_positive_control", "observer_CAS_contract",
     "cache_first_baseline_ledger", "remap_discards_inflight_hash", "canary_cleanup"})
     sv::record("write_watch_canary",name,"pass","synthetic analyzer fixture only");
 sv::live_publish_page(pt.get(),0x20,reinterpret_cast<uint8_t*>(uintptr_t(bytes.get())-0x20000));
 sv::live_publish_page(pt.get(),0x21,reinterpret_cast<uint8_t*>(uintptr_t(bytes.get()+4096)-0x21000));
 sv::live_publish_page(pt.get(),0x40,reinterpret_cast<uint8_t*>(uintptr_t(bytes.get())-0x40000));
 auto **write=sv::live_write_table(pt.get());assert(write);
 sv::request_benchmark();sv::on_render_end(tick);
 assert(sv::status_snapshot().phase_count==4 && sv::status_snapshot().total_ms==24000);
 for(unsigned phase=0;phase<4;++phase){
   assert(!sv::deduplicate_staging_cache()); // NEVER enables staging dedup here.
   for(unsigned i=0;i<150;++i){
     assert(sv::status_snapshot().phase==phase+1);
     if(phase==1 && i==30){++h1;assert(sv::live_write(pt.get(),0x40000,&h1,4)==sv::watch::Access::Complete);}
     if(phase==2 && i==50){++h2;std::memcpy(bytes.get()+4096,&h2,4);} // Bypass observer like raw host writer.
     hash(pt.get(),bytes.get());tick+=40;sv::on_render_end(tick);
   }
 }
 auto s=sv::status_snapshot();assert(s.stage==sv::Stage::Complete && s.outcome==sv::CaptureOutcome::Ready);
 assert(s.first_write_pages==1 && s.unobserved_changes==1 && s.phase_count==4);
 auto text=sv::make_hud_text(s,tick);assert(std::strstr(text.action.data(),"EXIT GAME NORMALLY TO SAVE"));
 assert(text.tone==sv::HudText::Tone::Warning && std::strstr(text.timing.data(),"no need to repeat"));
 auto bad_status=s;bad_status.outcome=sv::CaptureOutcome::Invalid;
 assert(sv::make_hud_text(bad_status,tick).tone==sv::HudText::Tone::Error);
 sv::frontend_poll(true);assert(sv::status_snapshot().stage==sv::Stage::Complete);
 std::filesystem::create_directory("report");sv::write_report("report");
 assert(write[0x20] && write[0x40]);sv::live_detach_table(pt.get());sv::end_session();
 sv::begin_session("NO_TABLE");sv::request_benchmark();sv::on_render_end(tick);
 for(unsigned i=0;i<600;++i){tick+=40;sv::on_render_end(tick);}
 assert(sv::status_snapshot().outcome==sv::CaptureOutcome::Inconclusive);
 sv::end_session();
}
''')
        exe=tmp/'controller'
        subprocess.run([a.cxx,'-std=c++20','-O2','-pthread','-Wall','-Wextra','-Werror','-pedantic',
                        '-D__SWITCH__','-DVITA3K_SWITCH_TRACELAB=1','-I',str(inc),'-I',str(ROOT/'vita3k/util/include'),
                        str(ROOT/'vita3k/util/src/switch_memory_validation.cpp'),str(tmp/'main.cpp'),'-o',str(exe)],check=True)
        subprocess.run([str(exe),str(tmp)],check=True,timeout=20)
        data=json.loads((tmp/'report/memory-validation.json').read_text())
        assert data['validation_revision']==4 and data['strict_hash'] and not data['hash_skipping']
        assert len(data['benchmark']['phases'])==4
        assert data['write_shadow']['phases'][1]['changed_cpu_seen']==1
        assert data['write_shadow']['phases'][2]['changed_unobserved']==1
        assert all(p['dedup_calls']==0 for p in data['benchmark']['phases'])
        spec=importlib.util.spec_from_file_location('analyzer',ROOT/'scripts/analyze-switch-memory-v3.py')
        m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
        report=m.analyze(data,2);assert report['comparison_eligible'] and len(report['pairs'])==2
        assert report['warnings'] and report['changed_without_observed_cpu']==1
        assert report['cache_ledger_check']['matched_to_telemetry'] is True
        assert report['cache_ledger_check']['reasons']['cpu_interval']==1
        assert report['cache_ledger_check']['reasons']['unobserved']==1
        bad=copy.deepcopy(data);bad['write_shadow']['cache_ledger']['changes']+=1
        assert not m.analyze(bad,2)['comparison_eligible']
        assert not m.analyze(data,3)['comparison_eligible']
        bad=copy.deepcopy(data);bad['tests']=[t for t in bad['tests'] if t['name']!='native_exact_store']
        assert not m.analyze(bad,2)['comparison_eligible']
        for mutate in ('phase','coverage','invalid','failed','duration','schema','no_full_hash'):
            x=copy.deepcopy(data)
            if mutate=='phase':x['benchmark']['phases'].pop()
            elif mutate=='coverage':x['write_shadow']['phases'][1]['covered_checks']=0
            elif mutate=='invalid':x['write_shadow']['phases'][2]['invalid_accesses']=1
            elif mutate=='failed':x['write_shadow']['failed']=True
            elif mutate=='duration':x['benchmark']['phases'][0]['duration_ms']=0
            elif mutate=='schema':x['write_shadow']['scope']='unknown'
            elif mutate=='no_full_hash':x['hash_skipping']=True
            r=m.analyze(x);assert not r['comparison_eligible'] and not r['pairs'],mutate
        subprocess.run(['python3',str(ROOT/'scripts/analyze-switch-memory-v3.py'),str(tmp/'report'),'--html',str(tmp/'report.html')],check=True,stdout=subprocess.DEVNULL)
    print('Live controller/HUD/export/analyzer: four phases, unknown writer captured, no upload dependency, failure exclusions OK')

if __name__=='__main__': main()
