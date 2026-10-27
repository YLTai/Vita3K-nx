// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
// SPDX-License-Identifier: GPL-2.0-or-later

#include <util/switch_thread.h>

#ifdef __SWITCH__
#include <util/log.h>
#include <switch/arm/counter.h>

#include <atomic>
#include <cstdio>
#include <cstdint>

namespace {
std::atomic<bool> requested{false};
std::atomic<bool> compatible{false};
std::atomic<bool> passthrough{false};
std::atomic<uint64_t> source_present_calls{0};
std::atomic<uint64_t> generated_frames{0};

// File creation, polling and closing are owned by the Switch frontend/main
// thread. Render/LSFG threads update only atomics; there is no SD-card I/O in
// the frame-generation hot path.
std::FILE *state_csv = nullptr;
unsigned flush_samples = 0;
} // namespace

void switch_cpu_profile_lsfg_stage(uint32_t stage, uint64_t begin_tick, uint64_t end_tick) {
    // Identical order to LsfgNxDebugStage in lsfg_bridge.h.
    if (stage > 7)
        return;
    switch_cpu_profile_render_phase(
        static_cast<SwitchCpuProfilePhase>(
            static_cast<uint8_t>(SwitchCpuProfilePhase::LsfgSlotFenceWait) + stage),
        begin_tick, end_tick);
}

void switch_cpu_profile_lsfg_state(bool wanted, bool supported, bool bypass) {
    requested.store(wanted, std::memory_order_relaxed);
    compatible.store(supported, std::memory_order_relaxed);
    passthrough.store(bypass, std::memory_order_relaxed);
    source_present_calls.fetch_add(1, std::memory_order_relaxed);
}

void switch_cpu_profile_lsfg_generated(uint32_t frames) {
    generated_frames.fetch_add(frames, std::memory_order_relaxed);
}

void switch_cpu_profile_lsfg_begin_session(const char *safe_title_id, int64_t epoch, uint64_t session) {
    if (state_csv) {
        std::fclose(state_csv);
        state_csv = nullptr;
    }
    requested.store(false, std::memory_order_relaxed);
    compatible.store(false, std::memory_order_relaxed);
    passthrough.store(false, std::memory_order_relaxed);
    source_present_calls.store(0, std::memory_order_relaxed);
    generated_frames.store(0, std::memory_order_relaxed);
    flush_samples = 0;

    char path[192]{};
    std::snprintf(path, sizeof(path),
        "sdmc:/switch/vita3k/logs/lsfg-state-%s-%lld-%llu.csv",
        safe_title_id, static_cast<long long>(epoch),
        static_cast<unsigned long long>(session));
    state_csv = std::fopen(path, "w");
    if (!state_csv) {
        LOG_WARN("[cpu-profile] Could not create LSFG state CSV: {}", path);
        return;
    }

    std::fputs(
        "elapsed_ms,requested,compatible,high_fps_passthrough,"
        "source_present_calls,generated_frames,actually_generated\n",
        state_csv);
    LOG_WARN("[cpu-profile] LSFG state timeline: {}", path);
}

void switch_cpu_profile_lsfg_poll(double elapsed_ms, bool final_sample) {
    if (!state_csv)
        return;
    const uint64_t sources = source_present_calls.exchange(0, std::memory_order_relaxed);
    const uint64_t generated = generated_frames.exchange(0, std::memory_order_relaxed);
    std::fprintf(state_csv, "%.2f,%d,%d,%d,%llu,%llu,%d\n",
        elapsed_ms,
        requested.load(std::memory_order_relaxed) ? 1 : 0,
        compatible.load(std::memory_order_relaxed) ? 1 : 0,
        passthrough.load(std::memory_order_relaxed) ? 1 : 0,
        static_cast<unsigned long long>(sources),
        static_cast<unsigned long long>(generated),
        generated > 0 ? 1 : 0);
    if (final_sample || (++flush_samples % 3) == 0)
        std::fflush(state_csv);
}

void switch_cpu_profile_lsfg_end_session() {
    if (!state_csv)
        return;
    std::fflush(state_csv);
    std::fclose(state_csv);
    state_csv = nullptr;
}
#endif
