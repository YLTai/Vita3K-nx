// Direct-Heap topology experiment. Never protects live Guest or imported memory.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <util/switch_deferred_import.h>

namespace switch_validation::direct_heap {
constexpr uint64_t page = 4096, align_bytes = 2 * 1024 * 1024;
constexpr uint64_t min_pool = 512ull * 1024 * 1024;
constexpr uint64_t reserve_bytes = 1024ull * 1024 * 1024;
// Same physical-memory policy as the Stack-backed allocator, without the
// unrelated Stack-region free-space ceiling. A failed allocation is not hidden.
constexpr uint64_t budget(uint64_t total, uint64_t committed) noexcept {
    if (committed < reserve_bytes + min_pool) return 0;
    const auto preferred = std::clamp<uint64_t>(total / 6, 768ull * 1024 * 1024,
        1280ull * 1024 * 1024);
    return std::min<uint64_t>(preferred, committed - reserve_bytes) & ~(align_bytes - 1);
}
struct Snapshot {
    bool active = false, finished = false, started = false;
    uint64_t pool_bytes = 0, commits = 0, committed_bytes = 0, recycles = 0, errors = 0;
    uint64_t ranges = 0, plain_bytes = 0, pinned_bytes = 0, other_bytes = 0, collisions = 0;
    uint64_t frames = 0, elapsed_ms = 0, remaining_ms = 30000;
};
struct State {
    std::atomic<bool> active{false}, finished{false}, started{false}, start_requested{false};
    std::atomic<bool> cache_attempted{false}, cache_passed{false}, cache_finished{false}, cache_cleanup_ok{false};
    std::atomic<uint64_t> pool_bytes{0}, commits{0}, committed_bytes{0}, recycles{0}, errors{0};
    std::atomic<uint64_t> ranges{0}, plain_bytes{0}, pinned_bytes{0}, other_bytes{0}, collisions{0};
    std::atomic<uint64_t> frames{0}, elapsed_ms{0};
    std::atomic_flag sampling = ATOMIC_FLAG_INIT;
    struct Key { uintptr_t begin = 0; size_t size = 0; uint64_t visits = 0, plain = 0, pinned = 0, other = 0;
        uint64_t final_plain = 0, final_pinned = 0, final_other = 0; };
    std::array<Key, 2048> keys{}; // First-seen input spans, not unique physical bytes.
    uint64_t first_tick = 0; // Render-thread owned.
    std::atomic<uint64_t> start_tick{0}, end_tick{0};
    std::atomic<bool> final_classification_complete{false};
    bool (*finish_census)() noexcept = nullptr; // Installed during pool activation, before workers.
};
inline State state;
// Normal control paths only. Never acquired by the exception handler.
// Serialize the two runtime scratch probes and their shared durable marker.
inline std::mutex scratch_probe_mutex;
// Caller has stopped prior session workers; pool allocation counters remain
// process-lifetime counters, while input samples/progress belong to this session.
inline void begin_session() noexcept {
    state.finished.store(false); state.started.store(false); state.start_requested.store(false);
    state.cache_attempted.store(false); state.cache_passed.store(false); state.cache_finished.store(false);
    state.cache_cleanup_ok.store(false); state.final_classification_complete.store(false); state.start_tick.store(0); state.end_tick.store(0);
    state.frames.store(0); state.elapsed_ms.store(0); state.first_tick = 0;
    state.ranges.store(0); state.plain_bytes.store(0); state.pinned_bytes.store(0);
    state.other_bytes.store(0); state.collisions.store(0); state.keys.fill({});
    state.sampling.clear(std::memory_order_release);
}
inline Snapshot snapshot() noexcept {
    Snapshot s;
    s.active = state.active.load(); s.finished = state.finished.load(); s.started = state.started.load();
    s.pool_bytes = state.pool_bytes.load(); s.commits = state.commits.load();
    s.committed_bytes = state.committed_bytes.load(); s.recycles = state.recycles.load();
    s.errors = state.errors.load(); s.ranges = state.ranges.load();
    s.plain_bytes = state.plain_bytes.load(); s.pinned_bytes = state.pinned_bytes.load();
    s.other_bytes = state.other_bytes.load(); s.collisions = state.collisions.load();
    s.frames = state.frames.load(); s.elapsed_ms = state.elapsed_ms.load();
    s.remaining_ms = s.elapsed_ms < 30000 ? 30000 - s.elapsed_ms : 0;
    return s;
}
inline void request_capture() noexcept { state.start_requested.store(true, std::memory_order_release); }
inline void frame(uint64_t tick, uint64_t hz) noexcept {
    if (!state.active.load() || !hz) return;
    if (state.start_requested.exchange(false, std::memory_order_acq_rel)) {
        // Reset input samples at the chosen scene on the render thread. A
        // concurrent sampler delays the reset; never clear its lock from here.
        if (state.sampling.test_and_set(std::memory_order_acquire)) {
            state.start_requested.store(true); return;
        }
        state.ranges.store(0); state.plain_bytes.store(0); state.pinned_bytes.store(0);
        state.other_bytes.store(0); state.collisions.store(0); state.keys.fill({});
        state.frames.store(0); state.elapsed_ms.store(0); state.finished.store(false);
        state.first_tick = tick; state.start_tick.store(tick); state.end_tick.store(0);
        state.final_classification_complete.store(false); deferred_import::registry.start_window();
        state.started.store(true); state.sampling.clear(std::memory_order_release);
    }
    if (!state.started.load() || state.finished.load()) return;
    ++state.frames;
    const auto ms = tick >= state.first_tick ? (tick - state.first_tick) * 1000 / hz : 0;
    state.elapsed_ms.store(ms);
    if (ms >= 30000) {
        // One bounded metadata-only census before GPU objects can be torn down.
        // A concurrent sampler delays DONE by a frame rather than racing its keys.
        if (state.finish_census && !state.finish_census()) return;
        state.end_tick.store(tick); deferred_import::registry.freeze_window(); state.finished.store(true);
    }
}
inline bool ready(const Snapshot &s) noexcept {
    return s.active && s.started && s.finished && s.frames >= 120 && s.ranges && s.commits && !s.errors;
}
inline void write_json(FILE *file) {
    const auto s = snapshot();
    std::fprintf(file, "{\"window_started\":%s,\"window_start_tick\":%llu,\"window_end_tick\":%llu,"
        "\"scratch_finished\":%s,\"scratch_cleanup_ok\":%s,",
        s.started ? "true" : "false", (unsigned long long)state.start_tick.load(),
        (unsigned long long)state.end_tick.load(), state.cache_finished.load() ? "true" : "false",
        state.cache_cleanup_ok.load() ? "true" : "false");
    std::fprintf(file, "\"active\":%s,\"finished\":%s,\"pool_bytes\":%llu,\"commits\":%llu,"
        "\"committed_bytes\":%llu,\"recycles\":%llu,\"errors\":%llu,\"input_ranges\":%llu,"
        "\"plain_heap_input_bytes\":%llu,\"device_shared_input_bytes\":%llu,\"other_input_bytes\":%llu,"
        "\"sample_collisions\":%llu,\"frames\":%llu,\"elapsed_ms\":%llu,"
        "\"sampling\":\"bounded first-seen spans; overlaps count again; not write-coverage proof\","
        "\"game_pages_protected\":false,\"game_hash_skipping\":false",
        s.active ? "true" : "false", s.finished ? "true" : "false",
        (unsigned long long)s.pool_bytes, (unsigned long long)s.commits,
        (unsigned long long)s.committed_bytes, (unsigned long long)s.recycles,
        (unsigned long long)s.errors, (unsigned long long)s.ranges,
        (unsigned long long)s.plain_bytes, (unsigned long long)s.pinned_bytes,
        (unsigned long long)s.other_bytes, (unsigned long long)s.collisions,
        (unsigned long long)s.frames, (unsigned long long)s.elapsed_ms);
    const bool locked = !state.sampling.test_and_set(std::memory_order_acquire);
    std::fprintf(file, ",\"input_details_complete\":%s,\"sampled_inputs\":[", locked ? "true" : "false");
    if (locked) {
        bool first = true;
        for (const auto &key : state.keys) if (key.begin) {
            std::fprintf(file, "%s{\"host\":%llu,\"bytes\":%zu,\"hash_observations\":%llu,"
                "\"first_plain_bytes\":%llu,\"first_pinned_bytes\":%llu,\"first_other_bytes\":%llu,"
                "\"final_plain_bytes\":%llu,\"final_pinned_bytes\":%llu,\"final_other_bytes\":%llu}",
                first ? "" : ",", (unsigned long long)key.begin, key.size,
                (unsigned long long)key.visits, (unsigned long long)key.plain,
                (unsigned long long)key.pinned, (unsigned long long)key.other,
                (unsigned long long)key.final_plain, (unsigned long long)key.final_pinned,
                (unsigned long long)key.final_other);
            first = false;
        }
        state.sampling.clear(std::memory_order_release);
    }
    std::fprintf(file, "],\"final_classification_complete\":%s,\"deferred_imports\":",
        state.final_classification_complete.load() ? "true" : "false");
    deferred_import::registry.write_json(file);
    std::fputs("}", file);
}
} // namespace switch_validation::direct_heap
