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
    bool active = false, finished = false;
    uint64_t pool_bytes = 0, commits = 0, committed_bytes = 0, recycles = 0, errors = 0;
    uint64_t ranges = 0, plain_bytes = 0, pinned_bytes = 0, other_bytes = 0, collisions = 0;
    uint64_t frames = 0, elapsed_ms = 0, remaining_ms = 30000;
};
struct State {
    std::atomic<bool> active{false}, finished{false}, cache_attempted{false}, cache_passed{false};
    std::atomic<uint64_t> pool_bytes{0}, commits{0}, committed_bytes{0}, recycles{0}, errors{0};
    std::atomic<uint64_t> ranges{0}, plain_bytes{0}, pinned_bytes{0}, other_bytes{0}, collisions{0};
    std::atomic<uint64_t> frames{0}, elapsed_ms{0};
    std::atomic_flag sampling = ATOMIC_FLAG_INIT;
    struct Key { uintptr_t begin = 0; size_t size = 0; };
    std::array<Key, 512> keys{}; // First-seen input spans, not unique physical bytes.
    uint64_t first_tick = 0; // Render-thread owned.
};
inline State state;
// Caller has stopped prior session workers; pool allocation counters remain
// process-lifetime counters, while input samples/progress belong to this session.
inline void begin_session() noexcept {
    state.finished.store(false); state.cache_attempted.store(false); state.cache_passed.store(false);
    state.frames.store(0); state.elapsed_ms.store(0); state.first_tick = 0;
    state.ranges.store(0); state.plain_bytes.store(0); state.pinned_bytes.store(0);
    state.other_bytes.store(0); state.collisions.store(0); state.keys.fill({});
    state.sampling.clear(std::memory_order_release);
}
inline Snapshot snapshot() noexcept {
    Snapshot s;
    s.active = state.active.load(); s.finished = state.finished.load();
    s.pool_bytes = state.pool_bytes.load(); s.commits = state.commits.load();
    s.committed_bytes = state.committed_bytes.load(); s.recycles = state.recycles.load();
    s.errors = state.errors.load(); s.ranges = state.ranges.load();
    s.plain_bytes = state.plain_bytes.load(); s.pinned_bytes = state.pinned_bytes.load();
    s.other_bytes = state.other_bytes.load(); s.collisions = state.collisions.load();
    s.frames = state.frames.load(); s.elapsed_ms = state.elapsed_ms.load();
    s.remaining_ms = s.elapsed_ms < 30000 ? 30000 - s.elapsed_ms : 0;
    return s;
}
inline void frame(uint64_t tick, uint64_t hz) noexcept {
    if (!state.active.load() || state.finished.load() || !hz) return;
    if (!state.first_tick) state.first_tick = tick;
    ++state.frames;
    const auto ms = tick >= state.first_tick ? (tick - state.first_tick) * 1000 / hz : 0;
    state.elapsed_ms.store(ms);
    if (ms >= 30000) state.finished.store(true);
}
inline bool ready(const Snapshot &s) noexcept {
    return s.active && s.finished && s.frames >= 120 && s.ranges && s.commits && !s.errors;
}
inline void write_json(FILE *file) {
    const auto s = snapshot();
    std::fprintf(file, "{\"active\":%s,\"finished\":%s,\"pool_bytes\":%llu,\"commits\":%llu,"
        "\"committed_bytes\":%llu,\"recycles\":%llu,\"errors\":%llu,\"input_ranges\":%llu,"
        "\"plain_heap_input_bytes\":%llu,\"device_shared_input_bytes\":%llu,\"other_input_bytes\":%llu,"
        "\"sample_collisions\":%llu,\"frames\":%llu,\"elapsed_ms\":%llu,"
        "\"sampling\":\"bounded first-seen spans; overlaps count again; not write-coverage proof\","
        "\"game_pages_protected\":false,\"game_hash_skipping\":false}",
        s.active ? "true" : "false", s.finished ? "true" : "false",
        (unsigned long long)s.pool_bytes, (unsigned long long)s.commits,
        (unsigned long long)s.committed_bytes, (unsigned long long)s.recycles,
        (unsigned long long)s.errors, (unsigned long long)s.ranges,
        (unsigned long long)s.plain_bytes, (unsigned long long)s.pinned_bytes,
        (unsigned long long)s.other_bytes, (unsigned long long)s.collisions,
        (unsigned long long)s.frames, (unsigned long long)s.elapsed_ms);
}
} // namespace switch_validation::direct_heap
