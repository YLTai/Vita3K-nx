// Horizon-only backing operations; policy/state stay host-testable.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <util/switch_direct_heap.h>
#if defined(__SWITCH__) && defined(VITA3K_SWITCH_TRACELAB)
#include <switch.h>
namespace switch_validation::direct_heap {
inline bool requested() {
    // Bootstrap precedes the main validator. Read only these independent keys;
    // no configuration/state changes are made after the pool has been reserved.
    bool off = false, legacy = false;
    if (FILE *f = std::fopen("sdmc:/switch/vita3k/tracelab.cfg", "r")) {
        char line[512];
        while (std::fgets(line, sizeof(line), f)) {
            char key[64]{}, value[64]{};
            if (std::sscanf(line, " %63[^= \t] = %63[^#; \t\r\n]", key, value) != 2) continue;
            if ((!std::strcmp(key, "mode") || !std::strcmp(key, "memory_validation"))
                && !std::strcmp(value, "off")) off = true;
            if (!std::strcmp(key, "guest_backing")) legacy = !std::strcmp(value, "stack_alias");
            if (!std::strcmp(key, "validation_experiment") &&
                (!std::strcmp(value, "write_audit") || !std::strcmp(value, "staging")
                 || !std::strcmp(value, "legacy_content_compare"))) off = true;
        }
        std::fclose(f);
    }
    return !off && !legacy;
}
inline bool writable_private(const void *pointer, size_t bytes) noexcept {
    const auto begin = reinterpret_cast<uintptr_t>(pointer);
    if (!begin || !bytes || (begin | bytes) % page || begin + bytes < begin) return false;
    for (uintptr_t pos = begin; pos < begin + bytes;) {
        MemoryInfo info{}; u32 pi = 0;
        if (R_FAILED(svcQueryMemory(&info, &pi, pos)) || (info.type & 0xff) != MemType_Heap
            || info.perm != Perm_Rw || info.attr != 0 || info.ipc_refcount || info.device_refcount
            || info.addr > pos || info.size > UINTPTR_MAX - info.addr || info.addr + info.size <= pos)
            return false;
        pos = std::min<uintptr_t>(begin + bytes, info.addr + info.size);
    }
    return true;
}
inline bool activate(void *pool, size_t bytes) noexcept {
    if (!writable_private(pool, bytes)) return false;
    state.pool_bytes.store(bytes); state.active.store(true); return true;
}
inline uint32_t commit(void *host, void *source, size_t bytes) noexcept {
    if (host != source || !writable_private(source, bytes)) { ++state.errors; return 0xd401; }
    ++state.commits; state.committed_bytes.fetch_add(bytes); return 0;
}
inline uint32_t recycle(void *host, void *source, size_t bytes) noexcept {
    // Do not recycle a still-imported/IPC/protected page, even if it is RW.
    // There is no attempt to clear DeviceShared or override driver ownership.
    if (host != source || !writable_private(source, bytes)) { ++state.errors; return 0xd401; }
    ++state.recycles; return 0;
}
inline void observe(const void *pointer, size_t bytes) noexcept {
    if (!state.active.load(std::memory_order_relaxed) || !state.started.load(std::memory_order_relaxed)
        || state.finished.load(std::memory_order_relaxed)
        || !pointer || !bytes) return;
    const auto begin = reinterpret_cast<uintptr_t>(pointer);
    if (begin + bytes < begin || state.sampling.test_and_set(std::memory_order_acquire)) return;
    struct Unlock { ~Unlock() { state.sampling.clear(std::memory_order_release); } } unlock;
    uint64_t mixed = begin ^ (uint64_t(bytes) * 0x9e3779b97f4a7c15ull);
    mixed = (mixed ^ (mixed >> 30)) * 0xbf58476d1ce4e5b9ull;
    mixed ^= mixed >> 27;
    State::Key *slot = nullptr;
    for (size_t probe = 0; probe < 8; ++probe) {
        auto &key = state.keys[(mixed + probe) % state.keys.size()];
        if (key.begin == begin && key.size == bytes) { ++key.visits; return; }
        if (!key.begin) { slot = &key; break; }
    }
    if (!slot) { ++state.collisions; return; }
    *slot = {begin, bytes, 1, 0, 0, 0}; ++state.ranges;
    uint64_t plain = 0, pinned = 0, other = 0;
    uintptr_t pos = begin;
    for (size_t steps = 0; pos < begin + bytes && steps < 256; ++steps) {
        MemoryInfo info{}; u32 pi = 0;
        if (R_FAILED(svcQueryMemory(&info, &pi, pos)) || info.addr > pos
            || info.size > UINTPTR_MAX - info.addr || info.addr + info.size <= pos) break;
        const auto end = std::min<uintptr_t>(begin + bytes, info.addr + info.size);
        if ((info.type & 0xff) == MemType_Heap && info.perm == Perm_Rw && !info.attr
            && !info.ipc_refcount && !info.device_refcount) plain += end - pos;
        else if (info.attr & 4u) pinned += end - pos;
        else other += end - pos;
        pos = end;
    }
    other += begin + bytes - pos;
    slot->plain = plain; slot->pinned = pinned; slot->other = other;
    state.plain_bytes.fetch_add(plain); state.pinned_bytes.fetch_add(pinned); state.other_bytes.fetch_add(other);
}
} // namespace switch_validation::direct_heap
#else
namespace switch_validation::direct_heap {
inline bool requested() { return false; }
inline bool writable_private(const void *, size_t) noexcept { return false; }
inline bool activate(void *, size_t) noexcept { return false; }
inline uint32_t commit(void *, void *, size_t) noexcept { return 0xd401; }
inline uint32_t recycle(void *, void *, size_t) noexcept { return 0xd401; }
inline void observe(const void *, size_t) noexcept {}
}
#endif
