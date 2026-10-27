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
#include <mutex>
#include <vector>

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
std::FILE *frame_csv = nullptr;
unsigned flush_samples = 0;

// Per-frame cadence needs raw timestamps: the existing 1 Hz phase summary
// cannot reconstruct P95/P99. The renderer and guest threads only append to
// this bounded RAM buffer. All SD writes happen during the frontend poll.
struct FrameEvent {
    uint64_t tick;
    uint64_t sequence;
    const char *name;
    uint64_t value;
};
constexpr size_t MAX_FRAME_EVENTS = 8192;
std::mutex frame_mutex;
std::vector<FrameEvent> frame_events;
std::atomic<bool> frame_recording{false};
uint64_t frame_origin_tick = 0;
uint64_t frame_sequence = 0;
uint64_t frame_dropped = 0;
} // namespace

void switch_cpu_profile_frame_event(const char *name, uint64_t value) {
    if (!frame_recording.load(std::memory_order_relaxed) || !name)
        return;
    const uint64_t tick = armGetSystemTick();
    std::lock_guard<std::mutex> lock(frame_mutex);
    if (!frame_recording.load(std::memory_order_relaxed))
        return;
    if (frame_events.size() >= MAX_FRAME_EVENTS) {
        ++frame_dropped;
        return;
    }
    frame_events.push_back(FrameEvent{tick, ++frame_sequence, name, value});
}

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
    // value: bit0=requested, bit1=compatible, bit2=high-fps bypass.
    switch_cpu_profile_frame_event("source_present",
        (wanted ? 1u : 0u) | (supported ? 2u : 0u) | (bypass ? 4u : 0u));
}

void switch_cpu_profile_lsfg_generated(uint32_t frames) {
    generated_frames.fetch_add(frames, std::memory_order_relaxed);
    // Completion of lsfg_nx_present(), not a real display scanout timestamp.
    switch_cpu_profile_frame_event("generated_return", frames);
}

void switch_cpu_profile_lsfg_begin_session(const char *safe_title_id, int64_t epoch, uint64_t session, uint64_t origin_tick) {
    frame_recording.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(frame_mutex);
        frame_events.clear();
        frame_events.reserve(MAX_FRAME_EVENTS);
        frame_dropped = 0;
        frame_sequence = 0;
        frame_origin_tick = origin_tick;
    }
    if (frame_csv) {
        std::fclose(frame_csv);
        frame_csv = nullptr;
    }
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

    std::snprintf(path, sizeof(path),
        "sdmc:/switch/vita3k/logs/frame-events-%s-%lld-%llu.csv",
        safe_title_id, static_cast<long long>(epoch),
        static_cast<unsigned long long>(session));
    frame_csv = std::fopen(path, "w");
    if (!frame_csv) {
        LOG_WARN("[cpu-profile] Could not create frame events CSV: {}", path);
        return;
    }
    std::fputs("elapsed_ms,sequence,event,value\n", frame_csv);
    frame_recording.store(true, std::memory_order_release);
    LOG_WARN("[cpu-profile] Per-frame cadence timeline: {}", path);
}

void switch_cpu_profile_lsfg_poll(double elapsed_ms, bool final_sample) {
    if (!state_csv && !frame_csv)
        return;
    if (state_csv) {
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
    }
    // Stop-the-world only for a short vector swap. The 1 Hz frontend does the
    // potentially expensive CSV formatting and SD writes outside this lock.
    std::vector<FrameEvent> snapshot;
    uint64_t dropped = 0;
    if (frame_csv) {
        {
            std::lock_guard<std::mutex> lock(frame_mutex);
            snapshot.swap(frame_events);
            dropped = frame_dropped;
            frame_dropped = 0;
        }
        const double ticks_to_ms = 1000.0 / static_cast<double>(armGetSystemTickFreq());
        for (const auto &event : snapshot)
            std::fprintf(frame_csv, "%.3f,%llu,%s,%llu\n",
                static_cast<double>(event.tick - frame_origin_tick) * ticks_to_ms,
                static_cast<unsigned long long>(event.sequence), event.name,
                static_cast<unsigned long long>(event.value));
        if (dropped)
            std::fprintf(frame_csv, "%.3f,0,events_dropped,%llu\n", elapsed_ms,
                static_cast<unsigned long long>(dropped));
    }
    const bool flush = final_sample || (++flush_samples % 3) == 0;
    if (flush) {
        if (state_csv)
            std::fflush(state_csv);
        if (frame_csv)
            std::fflush(frame_csv);
    }
}

void switch_cpu_profile_lsfg_end_session() {
    frame_recording.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(frame_mutex);
        frame_events.clear();
    }
    if (frame_csv) {
        std::fflush(frame_csv);
        std::fclose(frame_csv);
        frame_csv = nullptr;
    }
    if (state_csv) {
        std::fflush(state_csv);
        std::fclose(state_csv);
        state_csv = nullptr;
    }
}
#endif
