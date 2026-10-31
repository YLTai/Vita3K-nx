// Completion-scoped external writes for resource-hash reuse.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>

namespace switch_validation::reuse {
struct Span {
    uintptr_t address = 0;
    uint64_t bytes = 0;
    bool valid() const noexcept {
        return address && bytes && bytes <= std::numeric_limits<uintptr_t>::max() - address;
    }
    bool overlaps(const Span &b) const noexcept {
        if (!valid() || !b.valid()) return false;
        return address <= b.address ? b.address - address < bytes : address - b.address < b.bytes;
    }
};
enum class Writer : uint8_t { Host, Async, Gpu };
enum class Completion : uint8_t { HostWriteFinished, GpuHostVisible, Failed };
struct WriteTicket { uint64_t id = 0; explicit operator bool() const noexcept { return id != 0; } };
struct Registration { uint32_t slot = UINT32_MAX; uint64_t generation = 0; };
struct WriteStamp {
    uint64_t lifetime = 0, host = 0, async = 0, gpu = 0;
    bool valid = false, host_busy = false, async_busy = false, gpu_busy = false;
};

// The real producer registers the destination before publishing/performing the
// write. GPU tickets remain busy across RECORDING -> SUBMIT -> COMPLETION ->
// CPU visibility/cache maintenance. A fence signal alone does not retire them.
// This ledger records writes, including same-value writes, never content hashes.
// No locks are retained across a caller's I/O, GPU work or native/Guest code.
template<size_t Resources = 1024, size_t Writes = 256> class WriteLedger {
    struct Resource {
        Span pixels{}, palette{};
        uint64_t generation = 0;
        std::array<uint64_t, 3> version{};
        std::array<uint32_t, 3> busy{};
        bool used = false;
    };
    struct Pending { Span span{}; uint64_t id = 0; Writer kind = Writer::Host; bool all = false; };
    std::array<Resource, Resources> resources_{};
    std::array<Pending, Writes> writes_{};
    mutable std::mutex mutex_;
    uint64_t next_id_ = 1, next_generation_ = 1;
    bool failed_ = false;
    static bool hits(const Resource &r, const Pending &p) noexcept {
        return p.all || r.pixels.overlaps(p.span) || r.palette.overlaps(p.span);
    }
    void increment(uint64_t &v) noexcept {
        if (v == UINT64_MAX) failed_ = true; else ++v;
    }
public:
    Registration add(Span pixels, Span palette = {}) noexcept {
        std::lock_guard lock(mutex_);
        if (failed_ || !pixels.valid() || (palette.bytes && !palette.valid())
            || (!palette.bytes && palette.address) || next_generation_ == UINT64_MAX) {
            failed_ = true; return {};
        }
        for (uint32_t i = 0; i < resources_.size(); ++i) if (!resources_[i].used) {
            auto &r = resources_[i]; r = {}; r.used = true;
            r.pixels = pixels; r.palette = palette; r.generation = next_generation_++;
            // Interests registered after a producer's begin cannot miss it.
            for (const auto &p : writes_) if (p.id && hits(r, p)) ++r.busy[size_t(p.kind)];
            return {i, r.generation};
        }
        // Resource capacity is a local fallback, not corruption of prior leases.
        return {};
    }
    bool remove(Registration h) noexcept {
        std::lock_guard lock(mutex_);
        if (h.slot >= resources_.size() || !resources_[h.slot].used
            || resources_[h.slot].generation != h.generation) return false;
        resources_[h.slot] = {};
        return true;
    }
    WriteTicket begin(Writer kind, Span span = {}, bool unknown_range = false) noexcept {
        std::lock_guard lock(mutex_);
        if (failed_ || size_t(kind) >= 3 || (!unknown_range && !span.valid()) || next_id_ == UINT64_MAX) {
            failed_ = true; return {};
        }
        for (auto &p : writes_) if (!p.id) {
            p = {span, next_id_++, kind, unknown_range};
            for (auto &r : resources_) if (r.used && hits(r, p)) {
                increment(r.version[size_t(kind)]); ++r.busy[size_t(kind)];
            }
            return {p.id};
        }
        // Dropping a writer would fabricate an idle resource. Permanently deny
        // reuse for this ledger; the caller must continue the original full Hash.
        failed_ = true; return {};
    }
    bool finish(WriteTicket ticket, Completion completion) noexcept {
        std::lock_guard lock(mutex_);
        if (!ticket) { failed_ = true; return false; }
        for (auto &p : writes_) if (p.id == ticket.id) {
            const auto required = p.kind == Writer::Gpu ? Completion::GpuHostVisible : Completion::HostWriteFinished;
            if (completion != required) {
                // Keep busy on a failed or premature completion. No success
                // proof may escape simply because a request was dequeued.
                failed_ = true; return false;
            }
            for (auto &r : resources_) if (r.used && hits(r, p)) {
                if (!r.busy[size_t(p.kind)]) { failed_ = true; continue; }
                --r.busy[size_t(p.kind)]; increment(r.version[size_t(p.kind)]);
            }
            p = {}; return !failed_;
        }
        // Duplicate or foreign completion tokens are never accepted.
        failed_ = true; return false;
    }
    WriteStamp stamp(Registration h) const noexcept {
        std::lock_guard lock(mutex_);
        if (failed_ || h.slot >= resources_.size()) return {};
        const auto &r = resources_[h.slot];
        if (!r.used || r.generation != h.generation) return {};
        return {r.generation, r.version[0], r.version[1], r.version[2], true,
            r.busy[0] != 0, r.busy[1] != 0, r.busy[2] != 0};
    }
    bool failed() const noexcept { std::lock_guard lock(mutex_); return failed_; }
};
} // namespace switch_validation::reuse
