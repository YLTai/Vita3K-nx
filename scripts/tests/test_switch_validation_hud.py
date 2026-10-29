#!/usr/bin/env python3
"""Exercise the real controller + shared HUD presenter without a GPU or SVC."""
import argparse
import json
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--cxx', default='c++')
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='vita3k-hud-') as directory:
        tmp = Path(directory)
        inc = tmp / 'include'
        (inc / 'switch/arm').mkdir(parents=True)
        (inc / 'util').mkdir()
        (inc / 'switch/arm/counter.h').write_text('''#pragma once
#include <cstdint>
inline std::uint64_t armGetSystemTickFreq() { return 1000; }
''')
        (inc / 'util/log.h').write_text('''#pragma once
#define LOG_INFO(...) ((void)0)
#define LOG_WARN(...) ((void)0)
#define LOG_ERROR(...) ((void)0)
''')
        (tmp / 'test.cpp').write_text(r'''
#include <util/switch_memory_validation.h>
#include <util/switch_validation_schedule.h>
#include <cassert>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>
#include <atomic>
namespace sv = switch_validation;
uint64_t tick = 10000;
void config(const char *extra = "") {
    std::ofstream f("sdmc:/switch/vita3k/tracelab.cfg");
    f << "mode=auto\nmemory_validation=on\nvalidation_phase_seconds=5\nvalidation_settle_seconds=1\n" << extra;
}
void render(uint64_t delta, bool upload = true) {
    if (upload) sv::note_staging_publish(4096, sv::deduplicate_staging_cache());
    tick += delta;
    sv::on_render_end(tick);
}
void complete(bool upload = true) {
    for (unsigned phase = 0; phase < 8; ++phase) {
        for (unsigned i = 0; i < 150; ++i) {
            const auto before = sv::status_snapshot();
            assert(before.phase == phase + 1);
            assert(before.progress_per_mille < 1000);
            render(40, upload);
        }
    }
}
void report(const char *path) {
    std::filesystem::create_directory(path);
    sv::write_report(path);
}
int main(int argc, char **argv) {
    assert(argc == 2);
    std::filesystem::current_path(argv[1]);
    std::filesystem::create_directories("sdmc:/switch/vita3k");
    config();
    sv::begin_session("HUD");
    assert(sv::status_snapshot().stage == sv::Stage::Startup);
    sv::record("startup", "fake_pass", "pass", "host clock only");
    sv::record("startup", "fake_skip", "skip", "not run on host");
    render(40);
    auto s = sv::status_snapshot();
    assert(s.stage == sv::Stage::Waiting && s.passed == 1 && s.skipped == 2);
    assert(s.total_ms == 48000);
    sv::request_benchmark();
    assert(sv::status_snapshot().restart_pending);
    render(40);
    s = sv::status_snapshot();
    assert(s.stage == sv::Stage::Settling && s.run_id == 1 && s.phase == 1);
    assert(!s.restart_pending && s.remaining_ms == 48000 && s.stage_remaining_ms == 1000);
    // Advancing the UI clock alone must NEVER complete the run.
    const auto stale = sv::make_hud_text(s, tick + 1000000);
    assert(std::strstr(stale.title.data(), "WAITING FOR SOURCE FRAMES"));
    assert(!std::strstr(stale.title.data(), "DONE"));
    assert(sv::status_snapshot().stage == sv::Stage::Settling);
    render(1000);
    assert(sv::status_snapshot().stage == sv::Stage::Measuring);
    assert(sv::status_snapshot().stage_remaining_ms == 5000);
    sv::frontend_poll(true); // Must update the UI without another renderer tick.
    assert(sv::status_snapshot().stage == sv::Stage::Interrupted);
    const auto invalid = sv::make_hud_text(sv::status_snapshot(), tick);
    assert(std::strstr(invalid.title.data(), "INTERRUPTED"));
    assert(!std::strstr(invalid.title.data(), "DONE"));
    report("interrupted");
    sv::frontend_poll(false);
    sv::request_benchmark();
    render(40);
    assert(sv::status_snapshot().run_id == 2);
    std::atomic<bool> stop{false};
    // Reads race with phase publication, but never touch renderer-owned arrays.
    std::thread reader([&] {
        while (!stop.load()) {
            auto copy = sv::status_snapshot();
            assert(copy.progress_per_mille <= 1000);
            assert(copy.phase <= copy.phase_count);
            if (copy.stage == sv::Stage::Complete) assert(copy.phase == 8);
        }
    });
    complete();
    stop.store(true); reader.join();
    s = sv::status_snapshot();
    assert(s.stage == sv::Stage::Complete && s.outcome == sv::CaptureOutcome::Ready);
    assert(s.progress_per_mille == 1000 && s.remaining_ms == 0 && s.samples == 992);
    assert(s.dedup_calls == 496);
    auto text = sv::make_hud_text(s, tick);
    assert(std::strstr(text.title.data(), "CAPTURE READY"));
    assert(std::strstr(text.action.data(), "EXIT GAME NORMALLY TO SAVE"));
    assert(text.tone == sv::HudText::Tone::Ready);
    sv::frontend_poll(true); // Exit menu after completion must not invalidate success.
    assert(sv::status_snapshot().stage == sv::Stage::Complete);
    report("complete");
    sv::end_session();
    assert(sv::status_snapshot().stage == sv::Stage::Disabled);

    config("memory_validation=auto\nvalidation_warmup_seconds=2\n");
    sv::begin_session("AUTO"); render(40);
    assert(sv::status_snapshot().stage == sv::Stage::Warmup);
    assert(sv::status_snapshot().stage_remaining_ms == 2000);
    sv::frontend_poll(true);
    assert(sv::status_snapshot().stage == sv::Stage::Paused);
    render(1000); sv::frontend_poll(false); render(40);
    assert(sv::status_snapshot().stage_remaining_ms == 2000);
    render(2000); assert(sv::status_snapshot().stage == sv::Stage::Settling);
    sv::end_session();

    config(); sv::begin_session("NO_UPLOADS");
    sv::request_benchmark(); render(40, false); complete(false);
    s = sv::status_snapshot();
    assert(s.stage == sv::Stage::Complete && s.outcome == sv::CaptureOutcome::Inconclusive);
    text = sv::make_hud_text(s, tick);
    assert(text.tone == sv::HudText::Tone::Warning);
    assert(std::strstr(text.action.data(), "EXIT GAME NORMALLY TO SAVE"));
    report("no_uploads"); sv::end_session();

    config(); sv::begin_session("FAILED_TEST");
    sv::record("synthetic", "failure", "fail", "must never be a green DONE");
    sv::request_benchmark(); render(40); complete();
    assert(sv::status_snapshot().outcome == sv::CaptureOutcome::Invalid);
    assert(sv::make_hud_text(sv::status_snapshot(), tick).tone == sv::HudText::Tone::Error);
    report("failed"); sv::end_session();

    config(); sv::begin_session("TOO_FEW_FRAMES");
    sv::request_benchmark(); render(40);
    for (unsigned i=0; i<480; ++i) render(100);
    s = sv::status_snapshot();
    assert(s.stage == sv::Stage::Complete && s.insufficient_samples);
    assert(s.outcome == sv::CaptureOutcome::Invalid); // existing analyzer requires >=100/phase
    report("sparse"); sv::end_session();

    config(); sv::begin_session("STALL");
    sv::request_benchmark(); render(40); render(1000000);
    s = sv::status_snapshot();
    assert(s.stage == sv::Stage::Complete && s.outcome == sv::CaptureOutcome::Invalid);
    assert(s.insufficient_samples);
    report("stall"); sv::end_session();

    config("mode=off\n"); sv::begin_session("OFF");
    assert(!sv::enabled() && sv::status_snapshot().stage == sv::Stage::Disabled);
    sv::request_benchmark(); render(100000); report("off"); sv::end_session();
}
''')
        exe = tmp / 'hud-test'
        subprocess.run([args.cxx, '-std=c++20', '-O2', '-Wall', '-Wextra', '-Werror', '-pedantic', '-pthread',
                        '-D__SWITCH__', '-DVITA3K_SWITCH_TRACELAB=1', '-I', str(inc),
                        '-I', str(ROOT / 'vita3k/util/include'),
                        str(ROOT / 'vita3k/util/src/switch_memory_validation.cpp'),
                        str(tmp / 'test.cpp'), '-o', str(exe)], check=True)
        subprocess.run([str(exe), str(tmp)], check=True)
        for name, outcome in [('complete', 'ready'), ('interrupted', 'invalid'),
                              ('no_uploads', 'inconclusive'), ('failed', 'invalid'), ('sparse', 'invalid'), ('stall', 'invalid')]:
            report = json.loads((tmp / name / 'memory-validation.json').read_text())
            assert report['capture_outcome'] == outcome, (name, report)
            assert report['validation_revision'] == 3
            assert report['overlay_completion_is_not_graphics_validation']
        assert not (tmp / 'off/memory-validation.json').exists()
        # This must be attached independently of the user-controlled FPS widget.
        renderer = (ROOT / 'vita3k/renderer/src/renderer.cpp').read_text()
        assert renderer.index('memory_validation_overlay>();') < renderer.index('if (perf_overlay.enabled')
        cmake = (ROOT / 'vita3k/overlay/CMakeLists.txt').read_text()
        assert 'src/memory_validation_overlay.cpp' in cmake
    print('Validation HUD OK: live states, no timer-only DONE, concurrent snapshots, failures, exit menu, export')


if __name__ == '__main__':
    main()
