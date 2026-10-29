// Fault invalidation + no-scan reuse on isolated CPU Heap; NOT a live cache.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <util/switch_heap_fault_probe.h>
#include <util/switch_direct_heap.h>
#include <util/switch_memory_validation.h>
#include <cstddef>
#include <cstdint>
#if defined(__SWITCH__) && defined(VITA3K_SWITCH_TRACELAB)
#include <switch.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <malloc.h>
extern "C" void v3k_heap_store32(void *, uint64_t);
extern "C" void v3k_heap_store128(void *, uint64_t);
namespace switch_validation {
using ProtectedHash = uint64_t (*)(const void *, size_t);
inline void run_protected_cache_probe(ProtectedHash hash) {
    if (!heap_fault_probe_enabled() || !direct_heap::state.active.load()
        || direct_heap::state.cache_attempted.exchange(true)) return;
    const std::lock_guard<std::mutex> probe_lock(direct_heap::scratch_probe_mutex);
    if (!hash || !heap_fault_jit_allowed()) {
        record("heap_cache", "controls_complete", "fail", "native/JIT recovery prerequisite missing; no pages protected"); return;
    }
    constexpr size_t page = 4096, bytes = 4 * page, pixel_bytes = 3 * page, palette_bytes = 1024;
    auto *allocation = static_cast<uint8_t *>(memalign(page, bytes + 2 * page));
    if (!allocation) { record("heap_cache", "controls_complete", "fail", "scratch allocation failed"); return; }
    std::memset(allocation, 0xa5, bytes + 2 * page);
    auto *data = allocation + page;
    bool armed = false, ok = heap_fault_checkpoint("protected_cache_controls", true);
    uint64_t digest = 0, reference = 0, hits = 0, rebuilds = 0, audits = 0, faults = 0;
    const auto full = [&]() { return hash(data, pixel_bytes) ^ hash(data + pixel_bytes, palette_bytes); };
    const auto close = [&]() {
        if (!armed) return true;
        const auto r = heap_fault_disarm(); armed = false; faults += r.faults;
        return r.restored;
    };
    const auto get = [&]() {
        // One owner, one resource, no GPU/IPC, no concurrent producer. Real
        // kernel write faults invalidate this resource until a new RO baseline.
        // This is deliberately not a production concurrent rearm algorithm.
        if (armed) {
            const auto evidence = heap_fault_peek();
            if (!evidence.last_rc && evidence.dirty_mask == 0) { ++hits; return digest; }
        }
        if (!close() || !heap_fault_arm(data, bytes)) { ok = false; return uint64_t{0}; }
        armed = true; digest = full(); ++rebuilds; return digest;
    };
    const auto check = [&](const char *name, bool condition) {
        record("heap_cache", name, condition ? "pass" : "fail", "private CPU Heap; real source Hash; no live Guest reuse");
        ok &= condition;
    };
    if (ok) {
        const auto candidate = get(); reference = full(); ++audits;
        check("initial_baseline", ok && candidate == reference && rebuilds == 1);
    }
    if (ok) {
        bool equal = true;
        for (unsigned i = 0; i < 512; ++i) { const auto candidate = get(); const auto expected = full(); ++audits; equal &= candidate == expected; }
        check("clean_512_no_scan", equal && ok && hits == 512 && rebuilds == 1 && heap_fault_peek().faults == 0);
    }
    const auto changed = [&](const char *name, auto write, bool should_change) {
        if (!ok) return;
        const auto previous = digest, before = rebuilds;
        write();
        const bool seen = heap_fault_peek().dirty_mask != 0;
        const auto candidate = get(); const auto expected = full(); ++audits;
        check(name, seen && ok && rebuilds == before + 1 && candidate == expected
            && (should_change ? candidate != previous : candidate == previous));
    };
    changed("pixel_first", [&] { v3k_heap_store32(data, 0x10203040); }, true);
    changed("pixel_middle", [&] { v3k_heap_store32(data + page + 64, 0x11223344); }, true);
    changed("pixel_last", [&] { data[pixel_bytes - 1] ^= 1; }, true);
    changed("palette_only", [&] { v3k_heap_store32(data + pixel_bytes + 4, 0x55667788); }, true);
    changed("cross_page_store", [&] { v3k_heap_store128(data + page - 8, 0x0102030405060708ull); }, true);
    changed("host_memset", [&] { std::memset(data + page + 256, 0x3c, 256); }, true);
    changed("same_value_write", [&] { uint32_t v; std::memcpy(&v, data, 4); v3k_heap_store32(data, v); }, false);
    changed("same_page_outside_input", [&] { data[pixel_bytes + palette_bytes + 8] ^= 1; }, false);
    if (ok) {
        // Invalidate lifecycle identity before using the same address again.
        ok &= close();
        if (ok) {
            std::memset(data, 0x19, bytes);
            const auto before = rebuilds; const auto candidate = get(); const auto expected = full(); ++audits;
            check("same_address_new_lifetime", ok && rebuilds == before + 1 && candidate == expected);
        }
    }
    if (ok) {
        // Equal order and iteration count. Independent audits are outside timing.
        // This measures only this private 13-KiB input, not gameplay or a frame.
        volatile uint64_t sink = 0;
        for (unsigned i = 0; i < 64; ++i) { sink = full(); sink = get(); }
        for (unsigned sample = 0; sample < 4; ++sample) {
            const bool cached = sample == 1 || sample == 2;
            const auto start = armGetSystemTick();
            for (unsigned i = 0; i < 512; ++i) sink = cached ? get() : full();
            const auto elapsed = armGetSystemTick() - start;
            char name[48]; std::snprintf(name, sizeof(name), "private_ABBA_%u_%c", sample, cached ? 'B' : 'A');
            record("heap_cache", name, ok ? "pass" : "fail",
                "512 reads; A=full production digest B=clean fault-tracked reuse; no independent audit inside timing; not game speed", elapsed, 512);
        }
        (void)sink;
        const auto expected = full(); const auto candidate = get(); ++audits;
        check("post_timing_audit", ok && candidate == expected && heap_fault_peek().dirty_mask == 0);
    }
    ok &= close();
    const bool safe = heap_fault_safe_to_chainload();
    bool guards = safe;
    if (safe) for (size_t i = 0; i < page; ++i) guards &= allocation[i] == 0xa5 && data[bytes + i] == 0xa5;
    ok &= guards;
    char detail[256]; std::snprintf(detail, sizeof(detail),
        "hits=%llu rebuilds=%llu independent_audits=%llu faults=%llu safe=%u; single owner, known lifetime; GPU/IPC/concurrent rearm excluded",
        (unsigned long long)hits, (unsigned long long)rebuilds, (unsigned long long)audits, (unsigned long long)faults, unsigned(safe));
    ok &= heap_fault_checkpoint("protected_cache_returned", false);
    direct_heap::state.cache_passed.store(ok);
    record("heap_cache", "controls_complete", ok ? "pass" : "fail", detail);
    if (safe) std::free(allocation); // Never release unresolved RO storage.
}
} // namespace switch_validation
#else
namespace switch_validation {
inline void run_protected_cache_probe(uint64_t (*)(const void *, size_t)) {}
}
#endif
