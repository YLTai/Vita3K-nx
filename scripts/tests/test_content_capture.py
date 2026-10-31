#!/usr/bin/env python3
"""Compile actual content controller/API/memo/HUD and verify exported reports.
No mocked controller or memo; only Horizon clock/log and the Hash backend differ.
"""
import argparse
import copy
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[2]
SOURCE=r'''
#include <util/switch_memory_validation.h>
#include <util/switch_content_compare.h>
#include <util/content_hash_controls.h>
#include <util/switch_capture_plan.h>
#include <util/switch_write_watch_api.h>
#include <array>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <cstring>
namespace sv=switch_validation;
uint64_t tick=10000;
uint64_t hash(const void *p,size_t n){uint64_t h=1469598103934665603ULL;for(size_t i=0;i<n;++i){h^=static_cast<const uint8_t*>(p)[i];h*=1099511628211ULL;}return h;}
void controls(){for(auto name:sv::compact_controls)sv::record("write_watch_canary",name,"pass","controller-only fixture, NOT native execution");}
void report(const char*dir){std::filesystem::create_directory(dir);sv::write_report(dir);}
void render(){tick+=100;sv::on_render_end(tick);}
int main(int argc,char**argv){
 assert(argc==2);std::filesystem::current_path(argv[1]);std::filesystem::create_directories("sdmc:/switch/vita3k");
 std::ofstream("sdmc:/switch/vita3k/tracelab.cfg")<<"mode=auto\nvalidation_experiment=compact\nvalidation_phase_seconds=20\n";
 std::array<uint8_t,8192> pixels{};std::array<uint8_t,1024> palette{};
 sv::content::Input in{pixels.data(),pixels.size(),palette.data(),palette.size()};
 sv::begin_session("CONTENT");controls();assert(sv::content_compare_enabled());
 assert(!sv::live_write_table(nullptr));
 assert(sv::content_hash(in,hash)==sv::content::full_digest(in,hash)); // runs 12 actual controls
 sv::on_render_end(tick);auto s=sv::status_snapshot();assert(s.controls_complete && s.content_compare && s.total_ms==72000);
 assert(!sv::strict_hash());sv::request_benchmark();sv::on_render_end(tick);
 for(unsigned phase=0;phase<6;++phase){
  for(unsigned n=0;n<120;++n){
   assert(sv::strict_hash() && !sv::deduplicate_staging_cache());
   if(n==30)pixels[0]^=1;
   if(n==40)palette.back()^=1; // uninstrumented host/palette changes must still be visible
   assert(sv::content_hash(in,hash)==sv::content::full_digest(in,hash));render();
  }
 }
 s=sv::status_snapshot();assert(s.stage==sv::Stage::Complete && s.outcome==sv::CaptureOutcome::Ready);
 assert(s.audit_checks && s.compare_hits && !s.audit_disagreements && !s.shadow_bytes && !sv::strict_hash());
 assert(std::strstr(sv::make_hud_text(s,tick).action.data(),"SAVE"));
 sv::frontend_poll(true);assert(sv::status_snapshot().stage==sv::Stage::Complete);report("good");sv::end_session();
 // Deliberately mutate source during B's snapshot digest; restore full Hash and
 // stop immediately rather than displaying a false DONE or requesting a repeat.
 std::ofstream("sdmc:/switch/vita3k/tracelab.cfg")<<"mode=auto\nvalidation_experiment=content_compare\n";
 sv::begin_session("RACE");controls();sv::content_hash(in,hash);sv::request_benchmark();sv::on_render_end(tick);
 for(unsigned n=0;n<120;++n){sv::content_hash(in,hash);render();}
 sv::content::controls_detail::underlying=hash;sv::content::controls_detail::mutate=pixels.data();
 assert(sv::content_hash(in,sv::content::controls_detail::mutate_source)==sv::content::full_digest(in,hash));
 render();s=sv::status_snapshot();assert(s.stage==sv::Stage::Stopped && s.audit_disagreements==1 && !sv::strict_hash());
 sv::frontend_poll(true);assert(sv::status_snapshot().stage==sv::Stage::Stopped);
 sv::request_benchmark();assert(!sv::status_snapshot().restart_pending);
 report("failed");sv::end_session();
 std::ofstream("sdmc:/switch/vita3k/tracelab.cfg")<<"mode=off\n";
 sv::begin_session("OFF");assert(!sv::content_compare_enabled());assert(sv::content_hash(in,hash)==sv::content::full_digest(in,hash));sv::end_session();
}
'''
def main():
    p=argparse.ArgumentParser();p.add_argument('--cxx',default='c++');a=p.parse_args()
    with tempfile.TemporaryDirectory() as t:
        t=Path(t);inc=t/'include';(inc/'switch/arm').mkdir(parents=True);(inc/'util').mkdir()
        (inc/'switch/arm/counter.h').write_text('#pragma once\n#include <cstdint>\ninline uint64_t armGetSystemTickFreq(){return 1000;}\n')
        (inc/'util/log.h').write_text('#pragma once\n#define LOG_INFO(...) ((void)0)\n#define LOG_WARN(...) ((void)0)\n#define LOG_ERROR(...) ((void)0)\n')
        (t/'test.cpp').write_text(SOURCE)
        subprocess.run([a.cxx,'-std=c++20','-O2','-Wall','-Wextra','-Werror','-pedantic','-pthread',
            '-D__SWITCH__','-DVITA3K_SWITCH_TRACELAB=1','-DVITA3K_SWITCH_CLEAN_CAPTURE_DEFAULT=1',
            '-DVITA3K_SWITCH_WRITE_SHADOW_DEFAULT=1','-DVITA3K_SWITCH_CONTENT_COMPARE_DEFAULT=1',
            '-I',str(inc),'-I',str(ROOT/'vita3k/util/include'),str(t/'test.cpp'),str(ROOT/'vita3k/util/src/switch_memory_validation.cpp'),'-o',str(t/'test')],check=True)
        subprocess.run([str(t/'test'),str(t)],check=True,timeout=30)
        spec=importlib.util.spec_from_file_location('analyzer',ROOT/'scripts/analyze-switch-capture.py');mod=importlib.util.module_from_spec(spec);spec.loader.exec_module(mod)
        good=json.loads((t/'good/memory-validation.json').read_text());bad=json.loads((t/'failed/memory-validation.json').read_text())
        r=mod.analyze(good);assert r['comparison_eligible'],r
        assert r['comparison']['throughput_delta_pct']==0
        assert not mod.analyze(bad)['comparison_eligible']
        assert 'write_shadow' not in good and good['content_compare']['live_write_observer_enabled'] is False
        for change in ('audit','hit','control','ledger','overflow','boundary','mode','counts','bytes','memory'):
            d=copy.deepcopy(good);c=d['content_compare'];p=d['benchmark']['phases']
            if change=='audit':c['phases'][1]['audit_checks']=0
            if change=='hit':c['phases'][2]['hits']=0
            if change=='control':d['tests'].pop()
            if change=='ledger':c['phases'][1]['audit_disagreements']=1
            if change=='overflow':d['benchmark']['frames_overflow']=1
            if change=='boundary':c['phases'][1]['begin_tick']+=1
            if change=='mode':p[2]['variant']='full_hash'
            if change=='counts':p[0]['measured_intervals']=0
            if change=='bytes':c['phases'][2]['equal_bytes']=10**15
            if change=='memory':c['peak_shadow_bytes']=c['byte_budget']+1
            assert not mod.analyze(d)['comparison_eligible'],change
    print('Content capture: actual controller/API/memo/HUD, 12 native-hash control logic, all six phases, runtime audit fail-closed and analyzer exclusions passed (host Hash substituted)')
if __name__=='__main__':main()
