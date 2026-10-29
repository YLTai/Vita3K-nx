#!/usr/bin/env python3
"""Exercise the actual matrix driver with a host fixture, then its analyzer.

This is a scheduling/measurement-contract test, NOT a Dynarmic emulator or
native instruction test. Actual A64 codegen remains a separate preflight;
Run/Step and performance still require the Switch NRO.
"""
import argparse
import copy
import importlib.util
import json
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

HEAD = r'''
#include <util/switch_validation_bench.h>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>
using u64 = uint64_t;
using Result = uint32_t;
using TableMode = switch_validation::StoreBenchMode;
using switch_validation::store_mode_name;
constexpr uint32_t kDataPages=4, kPageSize=4096, kDataBase=0x20000;
constexpr uint32_t kWarmupStores=4096, kBenchStores=262144;
constexpr int InfoType_ThreadTickCount=25;
constexpr uint64_t TickCountInfo_Total=~uint64_t{0};
uint64_t tick=10000;
bool deny_cpu_ticks=false;
unsigned measured_run=0;
uint64_t armGetSystemTick() { return ++tick; }
int threadGetCurHandle() { return 3; }
int svcGetCurrentProcessorNumber() { return 1; }
Result svcGetInfo(u64 *out, int, int, uint64_t) {
    *out = ++tick;
    return deny_cpu_ticks ? 0xd401 : 0;
}
#define R_SUCCEEDED(x) ((x)==0)
struct MemoryFixture;
struct Config { MemoryFixture *memory; TableMode mode; bool boundaries; };
namespace Dynarmic::A32 {
struct Jit {
    Config config;
    std::array<uint32_t,16> regs{};
    unsigned measured=0;
    static inline unsigned live=0, max_live=0, created=0;
    explicit Jit(Config c) : config(c) { ++created; ++live; if(live>max_live) max_live=live; }
    ~Jit() { --live; }
    auto &Regs() { return regs; }
    void Run();
};
}
struct MemoryFixture {
    std::unique_ptr<int> read_table{new int{0}}, write_table{new int{0}};
    std::unique_ptr<uint8_t[]> bytes{new uint8_t[8]};
    Dynarmic::A32::Jit *active_jit=nullptr;
    uint64_t writes=0,reads=0,first_writes=0,invalid_writes=0;
    uint32_t dirty_mask=0,closed=0,bench=0x1000;
    void reset_bytes() {}
    void reset_events() { writes=reads=first_writes=invalid_writes=0; }
    bool arm(uint32_t mask) { closed=mask; dirty_mask=0; return true; }
    void prepare(Dynarmic::A32::Jit &jit,uint32_t,uint32_t) { active_jit=&jit; jit.regs.fill(0); }
    bool clean_stop() const { return true; }
    bool bench_contents_match() const { return true; }
};
void Dynarmic::A32::Jit::Run() {
    auto &m=*config.memory;
    assert(m.active_jit==this);
    const bool split=config.mode==TableMode::Mirror;
    if(split && m.closed) {
        assert(m.closed==15);
        m.writes=4; m.first_writes=4; m.dirty_mask=15; m.closed=0;
    }
    if(regs[2]==kBenchStores) {
        ++measured; ++measured_run;
        assert(measured<=15); // one shared split JIT: 3 modes x 5 rows
    }
    regs[2]=0;
    tick+=10000;
}
Config config_for(MemoryFixture &memory,TableMode mode,bool boundaries) { return {&memory,mode,boundaries}; }
namespace switch_validation {
unsigned records=0;
std::vector<std::string> modes;
void record(const char *suite,const char *name,const char *status,const char *detail,
    uint64_t elapsed=0,uint64_t iterations=0) {
    assert(std::string(suite)=="store_matrix_v2");
    assert(std::string(status)=="pass");
    if(std::string(name)=="scope") return;
    assert(elapsed>0 && iterations==kBenchStores);
    ++records;
    std::printf("%s\t%s\t%llu\t%llu\t%s\n",name,status,
        (unsigned long long)elapsed,(unsigned long long)iterations,detail);
}
}
'''

TAIL = r'''
int main(int argc,char**) {
    deny_cpu_ticks = argc>1;
    for (bool reverse : {false,true}) {
        for (unsigned row=0;row<5;++row) {
            unsigned seen=0;
            for(unsigned slot=0;slot<5;++slot) seen|=1u<<unsigned(switch_validation::store_bench_mode(row,slot,reverse));
            assert(seen==31);
        }
        for(unsigned slot=0;slot<5;++slot) {
            unsigned seen=0;
            for(unsigned row=0;row<5;++row) seen|=1u<<unsigned(switch_validation::store_bench_mode(row,slot,reverse));
            assert(seen==31);
        }
    }
    MemoryFixture memory;
    run_bench_matrix(memory);
    assert(memory.active_jit==nullptr);
    assert(measured_run==100 && switch_validation::records==100);
    assert(Dynarmic::A32::Jit::max_live==3 && Dynarmic::A32::Jit::live==0);
    assert(Dynarmic::A32::Jit::created==12); // never construct per sample
}
'''


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--cxx', default='c++')
    args = parser.parse_args()
    source = (ROOT / 'vita3k/cpu/src/switch_memory_validation.cpp').read_text()
    begin = source.index('void prepare_bench(')
    end = source.index('} // namespace', begin)
    spec = importlib.util.spec_from_file_location('matrix', ROOT / 'scripts/analyze-switch-memory-v2.py')
    analyzer = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(analyzer)
    with tempfile.TemporaryDirectory(prefix='vita3k-matrix-') as directory:
        tmp = Path(directory)
        cpp, exe = tmp / 'matrix.cpp', tmp / 'matrix'
        cpp.write_text(HEAD + source[begin:end] + TAIL)
        subprocess.run([args.cxx, '-std=c++20', '-O2', '-Wall', '-Wextra', '-Werror', '-pedantic',
                        '-I', str(ROOT / 'vita3k/util/include'), str(cpp), '-o', str(exe)], check=True)
        reports = []
        for extra in ([], ['no_cpu_query']):
            output = subprocess.run([str(exe)] + extra, check=True, text=True, capture_output=True).stdout
            tests = []
            for line in output.splitlines():
                name, status, elapsed, iterations, detail = line.split('\t')
                tests.append(dict(suite='store_matrix_v2', name=name, status=status,
                                  elapsed_ticks=int(elapsed), iterations=int(iterations), detail=detail))
            report = dict(timebase_hz=1000, validation_revision=2, tests_dropped=0, tests=tests)
            result = analyzer.analyze(report)
            assert result['observed_samples'] == result['accepted_samples'] == 100
            assert len(result['comparisons']) == 16
            assert all(c['complete'] and c['wall_ratio_median'] == 1 for c in result['comparisons'])
            assert all((c['cpu_bracket_ratio_median'] is None) == bool(extra) for c in result['comparisons'])
            reports.append(report)
        base = reports[0]
        mutations = [
            ('duplicate', lambda r: r['tests'].append(copy.deepcopy(r['tests'][0]))),
            ('missing', lambda r: r['tests'].pop(0)),
            ('failed', lambda r: r['tests'][0].update(status='fail')),
            ('migrated', lambda r: r['tests'][0].update(detail=r['tests'][0]['detail'].replace('core_after=1', 'core_after=2'))),
            ('wrong_callbacks', lambda r: r['tests'][0].update(detail=r['tests'][0]['detail'].replace('callbacks=0', 'callbacks=5'))),
            ('wrong_stores', lambda r: r['tests'][0].update(iterations=3)),
        ]
        for name, mutate in mutations:
            r = copy.deepcopy(base); mutate(r)
            result = analyzer.analyze(r)
            assert any(not c['complete'] and c['wall_ratio_median'] is None for c in result['comparisons']), name
        r = copy.deepcopy(base); r['tests_dropped'] = 1
        assert all(c['wall_ratio_median'] is None for c in analyzer.analyze(r)['comparisons'])
        # A different split JIT or a different pairing core must suppress ratios.
        for field, value in [('jit_object', '0x1234'), ('backing', '0x4321')]:
            r = copy.deepcopy(base)
            record = next(t for t in r['tests'] if 'mode=first_write_table ' in t['detail'])
            fields = analyzer.detail_fields(record['detail']); fields[field] = value
            record['detail'] = ' '.join(f'{k}={v}' for k, v in fields.items())
            assert any(not c['complete'] for c in analyzer.analyze(r)['comparisons'])
        assert not analyzer.analyze(dict(timebase_hz=1000, tests=[]))['matrix_available']
        path = tmp / 'memory-validation.json'
        path.write_text(json.dumps(base))
        cli = subprocess.run(['python3', str(ROOT / 'scripts/analyze-switch-memory-v2.py'), str(tmp)],
                             check=True, text=True, capture_output=True)
        assert json.loads(cli.stdout)['accepted_samples'] == 100
    print('Store matrix driver + analyzer OK (100 samples, three shared JITs, balanced rows, CPU-query failure, exclusions; no native JIT execution)')


if __name__ == '__main__':
    main()
