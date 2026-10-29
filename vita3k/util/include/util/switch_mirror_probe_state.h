// RAM-only progress for the automatic, private GPU ownership experiment.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <atomic>
#include <cstdint>
#include <cstdio>
namespace switch_validation::mirror_probe {
inline constexpr unsigned case_count = 14;
inline constexpr const char *names[case_count] = {
    "initial_upload", "clean_512_reuse", "cpu_first_word", "cpu_palette_word",
    "gpu_write_while_cpu_read_only", "gpu_then_cpu_partial", "same_value_cpu_overwrite",
    "cross_page_cpu_write", "cpu_dirty_then_gpu_partial", "gpu_to_cpu_read",
    "buffer_image_roundtrip", "same_address_new_lifetime", "final_integrity", "cleanup_complete"
};
struct Snapshot {
    bool started = false, finished = false, passed = false, cleanup = false;
    unsigned completed = 0;
    uint64_t uploaded = 0, downloaded = 0, audits = 0, faults = 0, waits = 0, ticks = 0;
};
struct State {
    std::atomic<bool> requested{false}, started{false}, finished{false}, passed{false}, cleanup{false};
    std::atomic<unsigned> completed{0};
    std::atomic<uint64_t> uploaded{0}, downloaded{0}, audits{0}, faults{0}, waits{0}, ticks{0};
};
inline State state;
// Only after prior renderer shutdown; no runner may reference the previous session.
inline void reset() noexcept {
    state.requested.store(false);state.started.store(false);state.finished.store(false);
    state.passed.store(false);state.cleanup.store(false);state.completed.store(0);
    state.uploaded.store(0);state.downloaded.store(0);state.audits.store(0);
    state.faults.store(0);state.waits.store(0);state.ticks.store(0);
}
inline Snapshot snapshot() {
    return {state.started.load(),state.finished.load(),state.passed.load(),state.cleanup.load(),
        state.completed.load(),state.uploaded.load(),state.downloaded.load(),state.audits.load(),
        state.faults.load(),state.waits.load(),state.ticks.load()};
}
inline void write_json(FILE *f) {
    const auto s=snapshot();
    std::fprintf(f,"{\"started\":%s,\"finished\":%s,\"passed\":%s,\"cleanup_complete\":%s,"
        "\"completed_cases\":%u,\"required_cases\":%u,\"upload_bytes\":%llu,\"download_bytes\":%llu,"
        "\"independent_byte_audits\":%llu,\"cpu_write_faults\":%llu,\"fence_waits\":%llu,\"elapsed_ticks\":%llu,"
        "\"scope\":\"private 16KiB CPU Heap + independent Vulkan buffers/image; transfer GPU writes; single owner; not live Guest or shader/IPC/concurrent coverage\"}",
        s.started?"true":"false",s.finished?"true":"false",s.passed?"true":"false",s.cleanup?"true":"false",
        s.completed,case_count,(unsigned long long)s.uploaded,(unsigned long long)s.downloaded,
        (unsigned long long)s.audits,(unsigned long long)s.faults,(unsigned long long)s.waits,(unsigned long long)s.ticks);
}
} // namespace switch_validation::mirror_probe
