// Narrow production contract: CPU/HLE sources; GPU-written inputs never reuse.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <util/resource_hash_reuse.h>
#include <util/resource_write_ledger.h>
#include <array>
#include <algorithm>
#include <cstdio>
#include <atomic>
#include <mutex>
namespace switch_validation::reuse {
// Sticky physical spans survive capture restart, unmap and address reuse. They
// clear only at session reset, after all old Guest/GPU workers have stopped.
// No fence, hash equality or elapsed time can release a vetoed destination.
class SourcePolicy {
    std::array<Span, 2048> veto_{};
    size_t used_ = 0;
    mutable std::mutex mutex_;
    bool backend_ = false;
    uint32_t deny_ = 0;
    uint64_t serial_ = 1, generation_ = 1, events_ = 0;
public:
    enum : uint32_t { ShaderStore = 1, Debugger = 2, Capacity = 4, InvalidRange = 8, Backend = 16, NativeNetwork = 32 };
    void reset() noexcept {
        std::lock_guard lock(mutex_);
        ++serial_; ++generation_; used_ = 0; events_ = 0; deny_ = 0; backend_ = false;
        if (!serial_ || !generation_) deny_ = Capacity;
    }
    void backend(bool supported) noexcept {
        std::lock_guard lock(mutex_); backend_ = supported;
        if (!supported) deny_ |= Backend;
    }
    uint32_t blockers() const noexcept {
        std::lock_guard lock(mutex_);
        return deny_ | (backend_ ? 0u : uint32_t(Backend));
    }
    void deny(uint32_t why) noexcept { std::lock_guard lock(mutex_); deny_ |= why; }
    void veto(const void *host, uint64_t bytes) noexcept {
        if (!bytes) return;
        std::lock_guard lock(mutex_); ++events_;
        Span s{reinterpret_cast<uintptr_t>(host), bytes};
        if (!s.valid()) { deny_ |= InvalidRange; return; }
        for (size_t i = 0; i < used_; ++i) {
            const auto &v = veto_[i];
            if (s.address >= v.address && s.address - v.address <= v.bytes
                && s.bytes <= v.bytes - (s.address - v.address)) return;
        }
        // Merge overlapping/adjacent ranges; no allocation in writer paths.
        for (size_t i = 0; i < used_;) {
            const auto v = veto_[i];
            if (s.overlaps(v) || s.address + s.bytes == v.address || v.address + v.bytes == s.address) {
                const auto begin = std::min(s.address, v.address);
                const auto end = std::max(s.address + s.bytes, v.address + v.bytes);
                s = {begin, end - begin}; veto_[i] = veto_[--used_];
            } else ++i;
        }
        if (used_ == veto_.size() || generation_ == UINT64_MAX) { deny_ |= Capacity; return; }
        veto_[used_++] = s; ++generation_;
    }
    struct Admission { uint64_t lifetime = 0, generation = 0; bool supported = false; };
    Admission admit(const Input &in) const noexcept {
        std::lock_guard lock(mutex_);
        Admission a{serial_, generation_, backend_ && !deny_ && in.valid()};
        if (a.supported) for (size_t i = 0; i < used_; ++i)
            if (veto_[i].overlaps({in.pixels,in.pixel_bytes}) || veto_[i].overlaps({in.palette,in.palette_bytes})) { a.supported = false; break; }
        return a;
    }
    void write(FILE *file) const {
        std::lock_guard lock(mutex_);
        std::fprintf(file,"{\"backend_supported\":%s,\"global_denials\":%u,\"veto_spans\":%zu,\"veto_events\":%llu,\"GPU_writers_reused\":false,\"veto_release\":\"session_end_only\"}",
            backend_?"true":"false",deny_,used_,(unsigned long long)events_);
    }
};
inline SourcePolicy source_policy;
} // namespace switch_validation::reuse
