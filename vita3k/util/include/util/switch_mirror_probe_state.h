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
    uint64_t shader_dispatches = 0, shader_fills = 0, shader_copies = 0, shader_chains = 0, guard_checks = 0;
};
struct State {
    std::atomic<bool> requested{false}, started{false}, finished{false}, passed{false}, cleanup{false};
    std::atomic<unsigned> completed{0};
    std::atomic<uint64_t> uploaded{0}, downloaded{0}, audits{0}, faults{0}, waits{0}, ticks{0};
    std::atomic<uint64_t> shader_dispatches{0}, shader_fills{0}, shader_copies{0}, shader_chains{0}, guard_checks{0};
};
inline State state;
// Only after prior renderer shutdown; no runner may reference the previous session.
inline void reset() noexcept {
    state.requested.store(false);state.started.store(false);state.finished.store(false);
    state.passed.store(false);state.cleanup.store(false);state.completed.store(0);
    state.uploaded.store(0);state.downloaded.store(0);state.audits.store(0);
    state.faults.store(0);state.waits.store(0);state.ticks.store(0);
    state.shader_dispatches.store(0);state.shader_fills.store(0);state.shader_copies.store(0);
    state.shader_chains.store(0);state.guard_checks.store(0);
}
inline Snapshot snapshot() {
    return {state.started.load(),state.finished.load(),state.passed.load(),state.cleanup.load(),
        state.completed.load(),state.uploaded.load(),state.downloaded.load(),state.audits.load(),
        state.faults.load(),state.waits.load(),state.ticks.load(),state.shader_dispatches.load(),
        state.shader_fills.load(),state.shader_copies.load(),state.shader_chains.load(),state.guard_checks.load()};
}
inline bool shader_verified(const Snapshot &s) noexcept {
    return s.shader_fills >= 5 && s.shader_copies >= 5
        && s.shader_dispatches == s.shader_fills + s.shader_copies
        && s.shader_chains == 1 && s.guard_checks >= 2;
}
inline void write_json(FILE *f) {
    const auto s=snapshot();
    std::fprintf(f,"{\"started\":%s,\"finished\":%s,\"passed\":%s,\"cleanup_complete\":%s,"
        "\"completed_cases\":%u,\"required_cases\":%u,\"upload_bytes\":%llu,\"download_bytes\":%llu,"
        "\"independent_byte_audits\":%llu,\"cpu_write_faults\":%llu,\"fence_waits\":%llu,\"elapsed_ticks\":%llu,"
        "\"probe_version\":2,\"shader_dispatches\":%llu,\"shader_fill_dispatches\":%llu,\"shader_copy_dispatches\":%llu,"
        "\"shader_chain_submissions\":%llu,\"gpu_guard_audits\":%llu,"
        "\"scope\":\"private 16KiB CPU Heap + guarded Vulkan buffers/image; real BDA compute stores and chained copies; single owner; not live Guest, attachment, IPC or concurrent coverage\"}",
        s.started?"true":"false",s.finished?"true":"false",s.passed?"true":"false",s.cleanup?"true":"false",
        s.completed,case_count,(unsigned long long)s.uploaded,(unsigned long long)s.downloaded,
        (unsigned long long)s.audits,(unsigned long long)s.faults,(unsigned long long)s.waits,(unsigned long long)s.ticks,
        (unsigned long long)s.shader_dispatches,(unsigned long long)s.shader_fills,(unsigned long long)s.shader_copies,
        (unsigned long long)s.shader_chains,(unsigned long long)s.guard_checks);
}
} // namespace switch_validation::mirror_probe
