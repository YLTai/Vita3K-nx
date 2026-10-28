#!/usr/bin/env python3
"""Compile the Switch TraceLab collector against minimal host stubs and test RAM-only export.

This is intentionally independent of devkitPro, Mesa, and the device runtime.
It checks C++ syntax, file lifecycle, schema integrity, NID aggregation, and
full contiguous frame recording before expensive NRO compilation starts.
"""
import argparse
import csv
import json
import shutil
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cxx", default="c++")
    args = parser.parse_args()

    with tempfile.TemporaryDirectory(prefix="vita3k-tracelab-") as directory:
        tmp = Path(directory)
        inc = tmp / "include"
        (inc / "switch" / "arm").mkdir(parents=True)
        (inc / "util").mkdir(parents=True)

        (inc / "switch.h").write_text(r'''
#pragma once
#include <cstdint>
extern std::uint64_t test_tick;
using u64 = std::uint64_t;
using Result = std::uint32_t;
constexpr int InfoType_ThreadTickCount = 1;
constexpr int TickCountInfo_Total = 4;
constexpr int CUR_THREAD_HANDLE = 5;
inline Result svcGetInfo(u64 *out, int, int, int) {
    *out = test_tick / 2;
    return 0;
}
inline std::uint32_t svcGetCurrentProcessorNumber() { return 0; }
#define R_FAILED(rc) ((rc) != 0)
''')
        (inc / "switch" / "arm" / "counter.h").write_text(r'''
#pragma once
#include <cstdint>
extern std::uint64_t test_tick;
inline std::uint64_t armGetSystemTick() { return test_tick; }
inline std::uint64_t armGetSystemTickFreq() { return 19200000ULL; }
''')
        (inc / "util" / "log.h").write_text(r'''
#pragma once
#define LOG_INFO(...) ((void)0)
#define LOG_WARN(...) ((void)0)
#define LOG_ERROR(...) ((void)0)
''')
        (tmp / "smoke.cpp").write_text(r'''
#include <util/switch_tracelab.h>
#include <switch/arm/counter.h>
#include <cassert>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
std::uint64_t test_tick = 100000;
int main(int argc, char **argv) {
    assert(argc == 2);
    std::filesystem::current_path(argv[1]);
    std::filesystem::create_directories("sdmc:/switch/vita3k");
    {
        std::ofstream cfg("sdmc:/switch/vita3k/tracelab.cfg");
        cfg << "mode=auto\nlegacy_csv=off\n"
               "unsafe_texture_hash_skip_interval=8\n"
               "unsafe_texture_hash_recheck_interval=8\n";
    }
    tracelab::begin_session("PCSD00035");
    assert(!tracelab::legacy_csv_enabled());
    assert(tracelab::unsafe_texture_hash_skip_interval() == 0);
    assert(tracelab::unsafe_texture_hash_recheck_interval() == 8);
    assert(tracelab::texture_hash_probe_interval() == 8);
    tracelab::tag_thread("test-guest");
    tracelab::cpu_checkpoint();
    for (std::uint32_t frame = 1; frame <= 100; ++frame) {
        for (int hle = 0; hle < 96; ++hle) {
            tracelab::Scope sample(tracelab::Metric::HleImport, tracelab::Domain::Cpu);
            test_tick += 130;
            if (sample.sampled())
                tracelab::nid_sample(0x0C7B834B, sample.elapsed_us());
        }
        {
            tracelab::Scope hashing(tracelab::Metric::TextureHash, tracelab::Domain::Render, 65536);
            test_tick += 350;
            tracelab::count(tracelab::Metric::TextureHashUnchanged);
            tracelab::count(tracelab::Metric::TextureHashAggressiveSkipped, 65536);
        }
        test_tick += 19200000ULL / 25;
        tracelab::event(tracelab::Event::RenderEnd, frame);
        tracelab::cpu_checkpoint();
        tracelab::poll();
    }
    // Aggressive mode quarantines individual changed textures while keeping
    // the mode active for other textures; both failure flags remain visible.
    tracelab::unsafe_texture_hash_recheck_mismatch();
    tracelab::unsafe_texture_hash_recheck_unverified();
    assert(tracelab::unsafe_texture_hash_recheck_interval() == 8);
    assert(tracelab::unsafe_texture_hash_skip_interval() == 0);
    // TraceLab must not create or write session files during gameplay.
    assert(!std::filesystem::exists("sdmc:/switch/vita3k/logs"));
    tracelab::end_session();
    assert(std::filesystem::exists("sdmc:/switch/vita3k/logs"));

    // The guest JIT write probe MUST disarm the old unsafe interval even
    // if the user left 64 enabled in the same config file.
    {
        std::ofstream cfg("sdmc:/switch/vita3k/tracelab.cfg");
        cfg << "mode=auto\nlegacy_csv=off\n"
               "guest_write_probe=texture\n"
               "texture_hash_probe_interval=0\n"
               "unsafe_texture_hash_skip_interval=8\n"
               "unsafe_texture_hash_recheck_interval=64\n";
    }
    tracelab::begin_session("WRITE_PROBE");
    assert(tracelab::guest_write_probe_mode() == 2);
    assert(tracelab::guest_write_probe_texture_pages());
    assert(tracelab::unsafe_texture_hash_skip_interval() == 0);
    assert(tracelab::unsafe_texture_hash_recheck_interval() == 0);
    tracelab::guest_write_probe_mark_texture(0x12000, 0x1800);
    tracelab::guest_write_probe_mark_texture(0x13000, 0x1800);
    assert(tracelab::guest_write_probe_page_is_texture(0x12000));
    assert(tracelab::guest_write_probe_page_is_texture(0x13000));
    assert(tracelab::guest_write_probe_page_is_texture(0x14000));
    assert(!tracelab::guest_write_probe_page_is_texture(0x15000));
    tracelab::count_many(tracelab::Metric::GuestJitFastWrites, 1000);
    tracelab::count_many(tracelab::Metric::GuestJitTexturePageWrites, 75);
    tracelab::count_many(tracelab::Metric::GuestJitSlowWrites, 30);
    for (std::uint32_t frame = 1; frame <= 20; ++frame) {
        test_tick += 19200000ULL / 25;
        tracelab::event(tracelab::Event::RenderEnd, frame);
        tracelab::poll();
    }
    tracelab::end_session();

    // Switching off all diagnostics does not create an extra session.
    {
        std::ofstream cfg("sdmc:/switch/vita3k/tracelab.cfg");
        cfg << "mode=off\nlegacy_csv=on\n";
    }
    tracelab::begin_session("OFF");
    assert(tracelab::legacy_csv_enabled());
    assert(tracelab::unsafe_texture_hash_skip_interval() == 0);
    assert(tracelab::unsafe_texture_hash_recheck_interval() == 0);
    assert(tracelab::texture_hash_probe_interval() == 0);
    assert(tracelab::guest_write_probe_mode() == 0);
    tracelab::event(tracelab::Event::RenderEnd, 999);
    tracelab::end_session();
    return 0;
}
''')
        exe = tmp / "trace-smoke"
        subprocess.run([
            args.cxx, "-std=c++23", "-Wall", "-Wextra", "-Werror", "-pedantic",
            "-D__SWITCH__", "-DVITA3K_SWITCH_TRACELAB=1",
            "-I", str(inc), "-I", str(ROOT / "vita3k/util/include"),
            str(ROOT / "vita3k/util/src/switch_tracelab.cpp"),
            str(tmp / "smoke.cpp"), "-o", str(exe)
        ], check=True, cwd=ROOT)
        subprocess.run([str(exe), str(tmp)], check=True)

        logs = tmp / "sdmc:/switch/vita3k/logs"
        sessions = [p for p in logs.iterdir() if p.is_dir()]
        assert len(sessions) == 2, sessions
        by_title = {
            json.loads((p / "manifest.json").read_text())["title"]: p
            for p in sessions
        }
        assert set(by_title) == {"PCSD00035", "WRITE_PROBE"}, by_title
        session = by_title["PCSD00035"]
        probe_session = by_title["WRITE_PROBE"]
        manifest = json.loads((session / "manifest.json").read_text())
        assert manifest["schema_version"] == 2, manifest
        assert manifest["guest_write_probe_mode"] == 0
        assert manifest["unsafe_texture_hash_skip_requested_interval"] == 0
        assert manifest["texture_hash_probe_interval"] == 8
        assert manifest["unsafe_texture_hash_skip_final_interval"] == 0
        assert manifest["unsafe_texture_hash_recheck_requested_interval"] == 8
        assert manifest["unsafe_texture_hash_recheck_final_interval"] == 8
        assert manifest["unsafe_texture_hash_recheck_mismatch_seen"] is True
        assert manifest["unsafe_texture_hash_recheck_unverified_seen"] is True
        assert manifest["unsafe_texture_hash_audit_mismatch_detected"] is False
        assert manifest["unsafe_texture_hash_audit_incomplete"] is False
        assert manifest["collector"] == "ram_only_until_exit", manifest
        assert not manifest["legacy_csv"], manifest
        assert manifest["frames_recorded"] == 100, manifest
        for key in ("frames_overflow", "events_dropped", "window_rows_overflow", "nid_samples_overflow"):
            assert manifest[key] == 0, (key, manifest)
        with (session / "frames.csv").open() as file:
            frames = list(csv.DictReader(file))
        assert len(frames) == 100, len(frames)
        assert [int(f["render_frame_id"]) for f in frames] == list(range(1, 101))
        with (session / "nid-samples.csv").open() as file:
            nid_rows = list(csv.DictReader(file))
        assert any(r["nid"].lower() == "0x0c7b834b" and int(r["samples"]) > 0
                   for r in nid_rows), nid_rows
        with (session / "threads.csv").open() as file:
            thread_rows = list(csv.DictReader(file))
        assert any(r["label"] == "test-guest" and int(r["cpu_query_ok"])
                   for r in thread_rows), thread_rows
        report = json.loads((session / "summary.json").read_text())
        metrics = {r["metric"]: r for r in report["metrics"]}
        assert metrics["hle_import"]["calls"] == 9600, metrics["hle_import"]
        assert metrics["texture_hash"]["bytes"] == 65536 * 100

        probe_manifest = json.loads((probe_session / "manifest.json").read_text())
        assert probe_manifest["guest_write_probe_mode"] == 2, probe_manifest
        assert probe_manifest["guest_write_probe_texture_pages"] == 3
        assert probe_manifest["unsafe_texture_hash_skip_requested_interval"] == 0
        assert probe_manifest["unsafe_texture_hash_recheck_requested_interval"] == 0
        probe_report = json.loads((probe_session / "summary.json").read_text())
        probe_metrics = {r["metric"]: r for r in probe_report["metrics"]}
        assert probe_metrics["guest_texture_pages_registered"]["calls"] == 3
        assert probe_metrics["guest_jit_fast_writes"]["calls"] == 1000
        assert probe_metrics["guest_jit_texture_page_writes"]["calls"] == 75
        assert probe_metrics["guest_jit_slow_writes"]["calls"] == 30

        # The PC report tool also checks the new RAM-first dataset when Node
        # is available, but Node is not a prerequisite for compiling the NRO.
        if shutil.which("node"):
            script = ROOT / "scripts/analyze-switch-tracelab.mjs"
            result = subprocess.run(
                ["node", str(script), str(session)],
                check=True, text=True, capture_output=True)
            analysis = json.loads(result.stdout)
            assert analysis["frames"]["frame_id_gaps"] == 0
            assert analysis["frames"]["count"] == 100
            assert analysis["trace"]["events_dropped"] == 0
            assert analysis["texture_hash_aggressive"]["requested_interval"] == 8
            assert analysis["texture_hash_aggressive"]["skipped_full_hashes"] == 100
            assert analysis["texture_hash_aggressive"]["per_texture_quarantine_seen"] is True

            probe_analysis = json.loads(subprocess.run(
                ["node", str(script), str(probe_session)],
                check=True, text=True, capture_output=True).stdout)
            assert probe_analysis["guest_write_probe"]["mode_name"] == "texture"
            assert probe_analysis["guest_write_probe"]["successful_all_counted_stores"] == 1030
            assert probe_analysis["guest_write_probe"]["ever_texture_pages_registered"] == 3

        print("TraceLab host smoke test OK (RAM-only, 2 sessions, guest writes, NID aggregation)")


if __name__ == "__main__":
    main()
