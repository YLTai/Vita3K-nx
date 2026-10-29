#!/usr/bin/env python3
"""Run the actual validation controller against a deterministic host clock.

Exercises phase boundaries, publication accounting, late transitions, pause /
restart, deferred export and a bounded recorder. No device SVC is simulated.
"""
import argparse
import json
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cxx", default="c++")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="vita3k-validation-") as directory:
        tmp = Path(directory)
        inc = tmp / "include"
        (inc / "switch/arm").mkdir(parents=True)
        (inc / "util").mkdir()
        (inc / "switch/arm/counter.h").write_text(r'''
#pragma once
#include <cstdint>
inline std::uint64_t armGetSystemTickFreq() { return 1000; }
''')
        (inc / "util/log.h").write_text(r'''
#pragma once
#define LOG_INFO(...) ((void)0)
#define LOG_WARN(...) ((void)0)
#define LOG_ERROR(...) ((void)0)
''')
        (tmp / "test.cpp").write_text(r'''
#include <util/switch_memory_validation.h>
#include <util/switch_validation_schedule.h>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <string>
namespace sv = switch_validation;
uint64_t tick = 10000;

void config(const char *extra) {
    std::ofstream out("sdmc:/switch/vita3k/tracelab.cfg");
    out << "mode=auto\nmemory_validation=on\nvalidation_phase_seconds=5\n"
           "validation_settle_seconds=1\n" << extra;
}
void render(uint64_t delta) {
    sv::note_staging_publish(4096, sv::deduplicate_staging_cache());
    tick += delta;
    sv::on_render_end(tick);
}
void report(const char *name) {
    std::filesystem::create_directory(name);
    sv::write_report(name);
}
void full_run() {
    for (unsigned phase = 0; phase < 8; ++phase) {
        for (unsigned n = 0; n < 150; ++n) {
            assert(sv::deduplicate_staging_cache() == sv::staging_dedup_order[phase]);
            render(40);
        }
    }
    assert(!sv::deduplicate_staging_cache());
}
int main(int argc, char **argv) {
    assert(argc == 2);
    std::filesystem::current_path(argv[1]);
    std::filesystem::create_directories("sdmc:/switch/vita3k");
    static_assert(!sv::phase_position(999, 1000, 5000).measuring);
    static_assert(sv::phase_position(1000, 1000, 5000).measuring);
    static_assert(sv::phase_position(6000, 1000, 5000).index == 1);
    static_assert(!sv::phase_position(6000, 1000, 5000).measuring);
    static_assert(sv::phase_position(48000, 1000, 5000).complete);
    static_assert(!sv::measured_interval(5999, 6000, 1000, 5000));
    static_assert(!sv::measured_interval(999, 1000, 1000, 5000));
    static_assert(sv::measured_interval(1000, 1040, 1000, 5000));

    config("");
    sv::begin_session("PCSD00035");
    assert(sv::enabled() && sv::strict_hash());
    std::string detail = "copied \"text\" \\ newline\n";
    sv::record("jit_selftest", "copy", "pass", detail.c_str(), 250, 10);
    detail.assign("mutated");
    sv::request_benchmark();
    sv::on_render_end(tick);
    full_run();
    assert(!std::filesystem::exists("sdmc:/switch/vita3k/logs"));
    assert(!std::filesystem::exists("complete"));
    report("complete");
    sv::end_session();

    // Initial menu pauses restart the auto warm-up instead of consuming it.
    config("memory_validation=auto\nvalidation_warmup_seconds=2\n");
    sv::begin_session("AUTO");
    render(40);
    render(1000);
    sv::frontend_poll(true);
    render(50);
    sv::frontend_poll(false);
    render(50); // New warm-up origin after the paused render tick.
    render(1960);
    report("waiting");
    render(40); // Automatic run starts here.
    render(1040);
    sv::frontend_poll(true);
    report("paused_exit"); // No renderer tick required to flag exit interruption.
    sv::frontend_poll(false);
    sv::request_benchmark();
    render(40); // Consume abort then start a new independent run.
    full_run();
    report("restarted");
    sv::end_session();

    // A stall crossing the whole scheduled settle must still allow actual
    // settling after policy changes. It cannot create an immediate B sample.
    config("");
    sv::begin_session("LATE");
    sv::request_benchmark();
    render(40);
    render(5900);
    render(3000); // Phase B entered 2.9s late, only now switches policy.
    assert(sv::deduplicate_staging_cache());
    for (int i = 0; i < 10; ++i) render(100);
    report("late_settle");
    render(100);
    report("late_measured");
    sv::end_session();

    config("");
    sv::begin_session("BOUNDED");
    for (int i = 0; i < 300; ++i)
        sv::record("jit_selftest", "bounded", "pass", "bounded recorder");
    report("bounded");
    sv::end_session();

    // mode=off overrides the automatic setting even when listed first.
    config("mode=off\nmemory_validation=auto\n");
    sv::begin_session("OFF");
    assert(!sv::enabled() && !sv::strict_hash());
    sv::request_benchmark();
    render(100000);
    report("off");
    sv::end_session();
}
''')
        exe = tmp / "validation-test"
        subprocess.run([
            args.cxx, "-std=c++20", "-O2", "-Wall", "-Wextra", "-Werror", "-pedantic",
            "-D__SWITCH__", "-DVITA3K_SWITCH_TRACELAB=1",
            "-I", str(inc), "-I", str(ROOT / "vita3k/util/include"),
            str(ROOT / "vita3k/util/src/switch_memory_validation.cpp"),
            str(tmp / "test.cpp"), "-o", str(exe)
        ], check=True)
        subprocess.run([str(exe), str(tmp)], check=True)

        def read(name):
            return json.loads((tmp / name / "memory-validation.json").read_text())

        complete = read("complete")
        assert complete["strict_hash"] and not complete["hash_skipping"]
        assert complete["tests_dropped"] == 0
        copied = next(t for t in complete["tests"] if t["name"] == "copy")
        assert copied["detail"] == 'copied "text" \\ newline\n'
        bench = complete["benchmark"]
        assert bench["completed"] and not bench["aborted"] and bench["run_id"] == 1
        assert "".join("B" if p["variant"] == "dedup" else "A"
                       for p in bench["phases"]) == "ABBABAAB"
        for phase in bench["phases"]:
            assert phase["complete"]
            assert phase["measured_intervals"] == 124, phase
            assert phase["duration_ms"] == 4960
            assert phase["loops_per_second"] == 25
            assert phase["p50_ms"] == phase["p99_ms"] == 40
            assert phase["publish_calls"] == 124
            assert phase["publish_bytes"] == 124 * 4096
            assert phase["dedup_calls"] == (124 if phase["variant"] == "dedup" else 0)
        assert not read("waiting")["benchmark"]["requested"]
        assert read("paused_exit")["benchmark"]["aborted"]
        restarted = read("restarted")["benchmark"]
        assert restarted["completed"] and not restarted["aborted"] and restarted["run_id"] == 2
        assert all(p["measured_intervals"] == 124 for p in restarted["phases"])
        assert read("late_settle")["benchmark"]["phases"][1]["measured_intervals"] == 0
        assert read("late_measured")["benchmark"]["phases"][1]["measured_intervals"] == 1
        bounded = read("bounded")
        assert len(bounded["tests"]) == 256 and bounded["tests_dropped"] == 45
        assert not (tmp / "off/memory-validation.json").exists()
    print("Memory validation controller OK (paired runs, pause/restart, late settle, RAM-only export)")


if __name__ == "__main__":
    main()
