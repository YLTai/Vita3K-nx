#!/usr/bin/env python3
"""Compile the real controller/runtime + real Table; deterministic host clock.
Native store dispatch is explicitly simulated from the write PTE (not A64).
The separate canary driver tests production control logic; CI builds native code.
"""
import argparse
import copy
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import zipfile
ROOT = Path(__file__).resolve().parents[2]

SOURCE = r'''
#include <util/switch_memory_validation.h>
#include <util/switch_write_watch_api.h>
#include <util/switch_capture_plan.h>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
namespace sv = switch_validation;
uint64_t tick = 10000, changes = 0;
uint32_t previous_hash = 0;
void hash(uint8_t **pt, uint8_t *host) {
    uint32_t value = 0; std::memcpy(&value, host, 4);
    auto token = sv::live_hash_begin(pt, 0x20000, host, 4, nullptr, 0);
    sv::live_hash_end(token, value);
    sv::live_cache_decision(1, 0x20000, 4, previous_hash ^ 1, value ^ 1, true);
    changes += value != previous_hash; previous_hash = value;
}
void report() {
    std::filesystem::create_directory("report");
    sv::write_report("report");
    std::ofstream("report/summary.json") << "{\"metrics\":[{\"metric\":\"texture_hash_changed\",\"calls\":" << changes << "}]}";
}
int main(int argc, char **argv) {
    assert(argc == 2); std::filesystem::current_path(argv[1]);
    std::filesystem::create_directories("sdmc:/switch/vita3k");
    // The new build must not inherit old 20s/3s/auto knobs.
    std::ofstream("sdmc:/switch/vita3k/tracelab.cfg")
        << "mode=auto\nmemory_validation=auto\nvalidation_experiment=write_shadow\n"
           "validation_warmup_seconds=120\nvalidation_phase_seconds=20\nvalidation_settle_seconds=3\n";
    auto raw = std::make_unique<uint8_t *[]>(1 << 20);
    std::unique_ptr<uint8_t, decltype(&std::free)> bytes(
        static_cast<uint8_t *>(std::aligned_alloc(4096, 8192)), &std::free);
    assert(bytes); std::memset(bytes.get(), 0, 8192);
    sv::begin_session("COMPACT");
    for (const char *name : sv::compact_controls)
        sv::record("write_watch_canary", name, "pass", "synthetic transcript for controller test, NOT native execution");
    sv::live_publish_page(raw.get(), 0x20, reinterpret_cast<uint8_t *>(uintptr_t(bytes.get()) - 0x20000));
    sv::live_publish_page(raw.get(), 0x40, reinterpret_cast<uint8_t *>(uintptr_t(bytes.get()) - 0x40000));
    auto **writes = sv::live_write_table(raw.get()); assert(writes);
    sv::on_render_end(tick);
    assert(sv::status_snapshot().stage == sv::Stage::Waiting);
    assert(sv::status_snapshot().total_ms == 72000 && !sv::strict_hash());
    // A UI wall-clock jump never completes a source-loop-controlled run.
    assert(sv::make_hud_text(sv::status_snapshot(), tick + 1000000).tone != sv::HudText::Tone::Ready);
    sv::request_benchmark(); sv::on_render_end(tick);
    const auto store = [&](uint32_t addr, uint32_t value) {
        const auto entry = writes[addr / 4096];
        if (entry) std::memcpy(reinterpret_cast<void *>(uintptr_t(entry) + addr), &value, 4);
        else assert(sv::live_write(raw.get(), addr, &value, 4) == sv::watch::Access::Complete);
    };
    for (unsigned phase = 0; phase < 6; ++phase) {
        const auto s = sv::status_snapshot();
        assert(s.phase == phase + 1 && s.policy_letter == sv::capture_mode_letter(sv::compact_order[phase]));
        for (unsigned n = 0; n < 120; ++n) {
            assert(sv::strict_hash() && !sv::deduplicate_staging_cache());
            if ((phase == 1 || phase == 2 || phase == 4) && (n == 30 || n == 40)) {
                store(0x40000, phase * 100 + n);
                assert(bool(writes[0x20]) == (phase != 2));
            }
            if (phase == 3 && n == 30) { uint32_t v=330; std::memcpy(bytes.get(), &v, 4); }
            if (phase == 3 && n == 40) {
                uint32_t v=340; std::memcpy(bytes.get(), &v, 4);
                sv::live_host_written(bytes.get(), 4, sv::watch::HostWriter::Memcpy);
            }
            if (phase == 3 && n == 50) {
                store(0x20010, 99); // Page neighbor: MUST NOT explain the raw content write.
                uint32_t v=350; std::memcpy(bytes.get(), &v, 4);
            }
            if (phase == 3 && n == 60) store(0x20000, 350); // Same value, no change.
            if (phase == 3 && n == 61) { uint32_t v=361; std::memcpy(bytes.get(), &v, 4); }
            if (phase == 2 && n == 20) {
                sv::live_publish_page(raw.get(), 0x20, raw[0x20]);
                assert(!writes[0x20]); // Identical PTE publication preserves guard.
            }
            hash(raw.get(), bytes.get()); tick += 100; sv::on_render_end(tick);
        }
    }
    auto s = sv::status_snapshot();
    assert(s.stage == sv::Stage::Complete && s.outcome == sv::CaptureOutcome::Ready);
    assert(s.controls_complete && s.phase_count == 6 && !sv::strict_hash());
    assert(writes[0x20] && writes[0x40]);
    sv::frontend_poll(true); assert(sv::status_snapshot().stage == sv::Stage::Complete);
    report(); sv::live_detach_table(raw.get()); sv::end_session();
    // Missing controls cannot be hidden by a green performance result.
    std::ofstream("sdmc:/switch/vita3k/tracelab.cfg") << "mode=auto\nvalidation_experiment=compact\n";
    sv::begin_session("MISSING");
    assert(!sv::status_snapshot().controls_complete);
    sv::request_benchmark(); sv::on_render_end(tick);
    for (unsigned n = 0; n < 720; ++n) {tick += 100; sv::on_render_end(tick);}
    assert(sv::status_snapshot().outcome == sv::CaptureOutcome::Invalid
        && sv::status_snapshot().stage == sv::Stage::Blocked && sv::status_snapshot().run_id == 0);
    sv::end_session();
    // Pauses retain interruption semantics; disabled mode is still respected.
    sv::begin_session("PAUSE");
    for (const char *name : sv::compact_controls) sv::record("write_watch_canary", name, "pass", "fixture");
    sv::request_benchmark(); sv::on_render_end(tick);
    sv::frontend_poll(true); tick+=100; sv::on_render_end(tick);
    assert(sv::status_snapshot().stage == sv::Stage::Interrupted && !sv::strict_hash());
    sv::end_session();
    std::ofstream("sdmc:/switch/vita3k/tracelab.cfg") << "mode=off\nvalidation_experiment=compact\n";
    sv::begin_session("OFF"); assert(!sv::enabled() && !sv::strict_hash()); sv::end_session();
}
'''


def main():
    parser=argparse.ArgumentParser();parser.add_argument('--cxx',default='c++');args=parser.parse_args()
    with tempfile.TemporaryDirectory() as directory:
        root=Path(directory);inc=root/'include';(inc/'switch/arm').mkdir(parents=True);(inc/'util').mkdir()
        (inc/'switch/arm/counter.h').write_text('#pragma once\n#include <cstdint>\ninline uint64_t armGetSystemTickFreq(){return 1000;}\n')
        (inc/'util/log.h').write_text('#pragma once\n#define LOG_INFO(...) ((void)0)\n#define LOG_WARN(...) ((void)0)\n#define LOG_ERROR(...) ((void)0)\n')
        (root/'main.cpp').write_text(SOURCE)
        subprocess.run([args.cxx,'-std=c++20','-O2','-Wall','-Wextra','-Werror','-pedantic','-pthread',
            '-D__SWITCH__','-DVITA3K_SWITCH_TRACELAB=1','-DVITA3K_SWITCH_CLEAN_CAPTURE_DEFAULT=1',
            '-I',str(inc),'-I',str(ROOT/'vita3k/util/include'),str(root/'main.cpp'),
            str(ROOT/'vita3k/util/src/switch_memory_validation.cpp'),'-o',str(root/'test')],check=True)
        subprocess.run([str(root/'test'),str(root)],check=True,timeout=30)
        data=json.loads((root/'report/memory-validation.json').read_text())
        assert data['capture_plan']=='compact-1' and data['controls_complete']
        observed=data['write_shadow']['phases']
        assert observed[1]['changed_cpu_seen']==1 and observed[1]['changed_unobserved']==1
        assert observed[2]['changed_cpu_seen']==2 and observed[2]['changed_unobserved']==0
        assert observed[3]['changed_unobserved']==3 and observed[3]['changed_host_seen']==1
        assert observed[3]['page_only_stores']==1
        assert observed[4]['changed_cpu_seen']==1 and observed[4]['changed_unobserved']==1
        spec=importlib.util.spec_from_file_location('compact',ROOT/'scripts/analyze-switch-capture.py')
        module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
        total=json.loads((root/'report/summary.json').read_text())['metrics'][0]['calls']
        report=module.analyze(data,total)
        assert report['comparison_eligible'],report['exclusions']
        assert report['controls_ok'] and report['cache_ledger_ok'] and len(report['comparisons'])==2
        assert report['changed_without_observed_writer']==5
        for mode,summary in report['groups'].items():assert summary['loops_per_second']==10,(mode,summary)
        for mutation in ('phase','count','control','budget','ledger','duration','invalid','ratio'):
            d=copy.deepcopy(data)
            if mutation=='phase': d['benchmark']['phases'].pop()
            elif mutation=='count': d['benchmark']['phases'][0]['measured_intervals']=49
            elif mutation=='control': d['tests'].pop()
            elif mutation=='budget': d['write_shadow']['phases'][2]['guard_budget_exhausted']=1
            elif mutation=='ledger': d['write_shadow']['cache_ledger']['changes']+=1
            elif mutation=='duration': d['benchmark']['phases'][0]['duration_ms']=8000
            elif mutation=='invalid': d['write_shadow']['phases'][0]['invalid_accesses']=1
            elif mutation=='ratio': d['benchmark']['phases'][0]['loops_per_second']=1000
            assert not module.analyze(d,total)['comparison_eligible'],mutation
        assert not module.analyze(data,None)['comparison_eligible']
        d=copy.deepcopy(data);d['write_shadow']['phases'][2]['guard_budget_exhausted']=1
        partial=module.analyze(d,total)
        assert partial['usable_comparisons']==['first_write'] and partial['phase_exclusions'][2]
        archive=root/'capture.zip'
        with zipfile.ZipFile(archive,'w') as z:
            for name in ('memory-validation.json','summary.json'):z.write(root/'report'/name,'session/'+name)
        d,n=module.load_session(archive);assert d==data and n==total
        subprocess.run(['python3',str(ROOT/'scripts/analyze-switch-capture.py'),str(archive),
                        '--html',str(root/'report.html')],check=True,stdout=subprocess.DEVNULL)
        assert (root/'report.html').exists()
    print('Compact capture passed: real controller/Table/runtime/HUD/export, 6 modes, stale-config reset, exact vs first-write, 15-control gate, analyzer rejection and ZIP input (no native A64 claim)')


if __name__=='__main__':main()
