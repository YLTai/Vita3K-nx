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
    auto a=sv::live_hash_begin(pt,0x20000,bytes,64,nullptr,0);sv::live_hash_end(a,h1);
    auto b=sv::live_hash_begin(pt,0x21000,bytes+4096,64,nullptr,0);sv::live_hash_end(b,h2);
    // Per-entry invalidation using the current receipt; fake history only in this host fixture.
    sv::live_texture_cache_changed(0x21000,64,h2-1,h2);
}
int main(int argc,char**argv){
 assert(argc==2);std::filesystem::current_path(argv[1]);
 std::filesystem::create_directories("sdmc:/switch/vita3k");
 {std::ofstream f("sdmc:/switch/vita3k/tracelab.cfg");f<<"mode=auto\nmemory_validation=on\nvalidation_experiment=write_shadow\nvalidation_phase_seconds=5\nvalidation_settle_seconds=1\n";}
 auto pt=std::make_unique<uint8_t*[]>(1<<20);
 std::unique_ptr<uint8_t,decltype(&std::free)> bytes(static_cast<uint8_t*>(std::aligned_alloc(4096,8192)),&std::free);
 assert(bytes);std::memset(bytes.get(),0,8192);
 sv::begin_session("LIVE");
 for(const char *name : {"native_in_range", "native_same_value", "page_false_share_then_payload", "native_alias",
     "native_palette_only", "negative_raw_host", "negative_hle_page_neighbor", "idempotent_publish_keeps_watch",
     "notice_during_hash", "mapping_during_hash", "phase_boundary_token", "callback_CAS_success_failure",
     "native_cross_page", "scratch_guard_pages", "cache_baseline_reconciliation", "cache_history_reconciliation",
     "cache_missing_receipt", "cache_mutated_receipt", "selected_hle_notice_0", "selected_hle_notice_1",
     "selected_hle_notice_2", "selected_hle_notice_3", "selected_hle_notice_4", "selected_hle_notice_5"})
   sv::record("observer_challenge",name,"pass","synthetic report fixture; observer logic tested separately");
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
     if(phase==1 && i==60){++h1;std::memcpy(bytes.get(),&h1,4);
        sv::live_hle_write_completed(pt.get(),bytes.get(),4,sv::watch::HleHook::LibcMemcpy);}
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
        assert data['write_shadow']['phases'][1]['changed_cpu_seen']==2
        assert data['write_shadow']['phases'][2]['changed_unobserved']==1
        assert data['write_shadow']['phases'][1]['changed_cpu_and_hle']==1
        assert data['write_shadow']['phases'][1]['hle_hooks'][0]==1
        assert data['write_shadow']['phases'][1]['cache_changes']==150
        assert data['write_shadow']['phases'][1]['cache_baseline_changes']==1
        assert all(p['dedup_calls']==0 for p in data['benchmark']['phases'])
        spec=importlib.util.spec_from_file_location('analyzer',ROOT/'scripts/analyze-switch-memory-v3.py')
        m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
        report=m.analyze(data);assert report['comparison_eligible'] and len(report['pairs'])==2
        assert report['warnings'] and report['changed_without_observed_cpu']==1
        for mutate in ('phase','coverage','invalid','failed','duration','schema','no_full_hash','challenge','hook_counts'):
            x=copy.deepcopy(data)
            if mutate=='phase':x['benchmark']['phases'].pop()
            elif mutate=='coverage':x['write_shadow']['phases'][1]['covered_checks']=0
            elif mutate=='invalid':x['write_shadow']['phases'][2]['invalid_accesses']=1
            elif mutate=='failed':x['write_shadow']['failed']=True
            elif mutate=='duration':x['benchmark']['phases'][0]['duration_ms']=0
            elif mutate=='schema':x['write_shadow']['scope']='unknown'
            elif mutate=='no_full_hash':x['hash_skipping']=True
            elif mutate=='challenge':x['tests']=[t for t in x['tests'] if t['name']!='negative_raw_host']
            elif mutate=='hook_counts':x['write_shadow']['phases'][1]['hle_calls']+=1
            r=m.analyze(x);assert not r['comparison_eligible'] and not r['pairs'],mutate
        subprocess.run(['python3',str(ROOT/'scripts/analyze-switch-memory-v3.py'),str(tmp/'report'),'--html',str(tmp/'report.html')],check=True,stdout=subprocess.DEVNULL)
    print('Live controller/HUD/export/analyzer: four phases, unknown writer captured, no upload dependency, failure exclusions OK')

if __name__=='__main__': main()
