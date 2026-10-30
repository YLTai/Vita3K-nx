// Non-blocking grace periods for JIT page-table publication. No store instrumentation.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <limits>
#include <mutex>
#include <util/hle_range_gate.h>

namespace switch_validation::epoch {
struct Stamp {
    uint64_t ticket = 0, external = 0, hle_active = 0;
    bool drained = false, valid = false;
    RangeStamp ranges;
};
struct Stats {
    uint64_t publications = 0, entries = 0, exits = 0, active = 0;
    uint64_t host_entries = 0, host_exits = 0, host_active = 0, gpu_boundaries = 0;
    bool failed = false;
};
// Lock order: observer metadata -> domain. Run/HLE leases never retain this
// mutex while executing code, waiting, making SVCs, or calling the observer.
class Domain {
    mutable std::mutex mutex_;
    std::array<uint64_t, 64> readers_{};
    uint64_t generation_ = 1, external_ = 0;
    mutable uint64_t drained_through_ = 0;
    Stats stats_;
    HleRanges ranges_;
public:
    static constexpr unsigned invalid_slot = 64;
    bool reset() {
        std::lock_guard lock(mutex_);
        if (stats_.active || stats_.host_active) { stats_.failed = true; return false; }
        if (!ranges_.reset()) { stats_.failed = true; return false; }
        readers_.fill(0); generation_ = 1; external_ = drained_through_ = 0; stats_ = {}; return true;
    }
    unsigned enter() {
        std::lock_guard lock(mutex_);
        for (unsigned i = 0; i < readers_.size(); ++i) if (!readers_[i]) {
            readers_[i] = generation_; ++stats_.entries; ++stats_.active; return i;
        }
        stats_.failed = true; return invalid_slot;
    }
    void leave(unsigned slot) {
        std::lock_guard lock(mutex_);
        if (slot >= readers_.size() || !readers_[slot]) { stats_.failed = true; return; }
        readers_[slot] = 0; ++stats_.exits; --stats_.active;
    }
    // Caller publishes all trapping PTEs BEFORE this operation. Acquiring the
    // same mutex at subsequent JIT entry makes those stores visible. Any JIT
    // already running at publication must EXIT before absence is evidence.
    // This never waits for a JIT: a busy reader only makes a sample ineligible.
    uint64_t publish() {
        std::lock_guard lock(mutex_);
        if (generation_ == std::numeric_limits<uint64_t>::max()) { stats_.failed = true; return 0; }
        ++stats_.publications; return ++generation_;
    }
    Stamp stamp(uint64_t ticket, unsigned resource = HleRanges::no_slot) const {
        std::lock_guard lock(mutex_);
        bool drained = ticket && ticket <= generation_ && !stats_.failed && !ranges_.failed();
        // Once a ticket is drained, no future entrant can predate it. Avoid
        // scanning 64 slots on every unchanged texture Hash after that point.
        if (drained && ticket > drained_through_) {
            for (uint64_t reader : readers_) if (reader && reader < ticket) drained = false;
            if (drained) drained_through_ = ticket;
        }
        return {ticket, external_, stats_.host_active, drained, !stats_.failed && !ranges_.failed(), ranges_.stamp(resource)};
    }
    unsigned host_enter(uint32_t nid = 0) {
        std::lock_guard lock(mutex_);
        ++stats_.host_entries; ++stats_.host_active; advance_external();
        return ranges_.enter(nid);
    }
    void host_leave(unsigned slot, const char *name = nullptr) {
        std::lock_guard lock(mutex_);
        if (!stats_.host_active) { stats_.failed = true; return; }
        ++stats_.host_exits; --stats_.host_active; advance_external();
        ranges_.name(slot, name); ranges_.leave(slot);
    }
    void gpu_boundary() {
        std::lock_guard lock(mutex_);
        ++stats_.gpu_boundaries; advance_external(); ranges_.gpu_boundary();
    }
    Stats snapshot() const { std::lock_guard lock(mutex_); auto s = stats_; s.failed |= ranges_.failed(); return s; }
    void name_import(unsigned slot, const char *name) { std::lock_guard lock(mutex_); ranges_.name(slot, name); }
    bool declare_import(unsigned slot, const char *name, std::initializer_list<const char *> names,
        std::initializer_list<HostSpan> spans, bool prior_unknown) {
        std::lock_guard lock(mutex_); ranges_.name(slot, name); return ranges_.declare(slot, names, spans, prior_unknown);
    }
    bool park_import(unsigned slot, const char *name) {
        std::lock_guard lock(mutex_); return ranges_.park(slot, name);
    }
    void resume_import(unsigned slot) { std::lock_guard lock(mutex_); ranges_.resume(slot); }
    unsigned begin_output(HostSpan span, unsigned source, bool valid) {
        std::lock_guard lock(mutex_); return ranges_.begin_output(span, source, valid);
    }
    void end_output(unsigned slot) { std::lock_guard lock(mutex_); ranges_.end_output(slot); }
    HleRanges::OutputStats output_stats() const { std::lock_guard lock(mutex_); return ranges_.output_stats(); }
    void clear_interests() { std::lock_guard lock(mutex_); ranges_.clear_interests(); }
    void watch_range(unsigned id, const void *pixels, uint64_t n, const void *palette, uint64_t m) {
        std::lock_guard lock(mutex_); ranges_.watch(id, {pixels, n}, {palette, m});
    }
    void explain_range(unsigned id) { std::lock_guard lock(mutex_); ranges_.explain(id); }
    void write_ranges(FILE *file) const { std::lock_guard lock(mutex_); ranges_.write_json(file); }
private:
    void advance_external() {
        if (external_ == std::numeric_limits<uint64_t>::max()) stats_.failed = true;
        else ++external_;
    }
};
inline Domain domain;
inline std::atomic<bool> enabled{false};
class RunLease {
    Domain *domain_ = nullptr;
    unsigned slot_ = Domain::invalid_slot;
public:
    explicit RunLease(Domain *d = enabled.load(std::memory_order_acquire) ? &domain : nullptr) : domain_(d) {
        if (domain_) slot_ = domain_->enter();
    }
    ~RunLease() { if (domain_ && slot_ != Domain::invalid_slot) domain_->leave(slot_); }
    RunLease(const RunLease &) = delete;
    RunLease &operator=(const RunLease &) = delete;
};
class NativeWaitLease;
class HostLease {
    friend class NativeWaitLease;
    Domain *domain_ = nullptr;
    unsigned slot_ = HleRanges::no_slot;
    HostLease *previous_ = nullptr;
    const char *name_ = nullptr;
    inline static thread_local HostLease *current_ = nullptr;
public:
    explicit HostLease(Domain *d = enabled.load(std::memory_order_acquire) ? &domain : nullptr, uint32_t nid = 0)
        : domain_(d), previous_(current_) {
        if (domain_) slot_ = domain_->host_enter(nid);
        current_ = this;
    }
    ~HostLease() {
        current_ = previous_;
        if (domain_) domain_->host_leave(slot_, name_);
    }
    static void name_current(const char *name) {
        // Thread-local metadata only: naming a frequent HLE must not add a mutex.
        if (current_ && current_->domain_ && !current_->name_) current_->name_ = name;
    }
    static bool declare_current(std::initializer_list<const char *> names,
        std::initializer_list<HostSpan> spans, bool prior_unknown) {
        return current_ && current_->domain_
            && current_->domain_->declare_import(current_->slot_, current_->name_, names, spans, prior_unknown);
    }
    HostLease(const HostLease &) = delete;
    HostLease &operator=(const HostLease &) = delete;
};
// Must surround ONLY a native host wait with no Guest-memory-writing predicate,
// not an HLE body, queue insertion, callback or submission. A scoped output
// contract is deliberately NOT suspended. Prefix/suffix and other producers
// remain tracked; the guard restores unknown status before the caller resumes.
class NativeWaitLease {
    Domain *domain_ = nullptr;
    unsigned slot_ = HleRanges::no_slot;
public:
    NativeWaitLease() {
        const auto *h = HostLease::current_;
        if (h && h->domain_ && h->domain_->park_import(h->slot_, h->name_)) {
            domain_ = h->domain_; slot_ = h->slot_;
        }
    }
    ~NativeWaitLease() { if (domain_) domain_->resume_import(slot_); }
    NativeWaitLease(const NativeWaitLease &) = delete;
    NativeWaitLease &operator=(const NativeWaitLease &) = delete;
};
inline void name_import(const char *name) { HostLease::name_current(name); }
inline bool declare_outputs(std::initializer_list<const char *> names,
    std::initializer_list<HostSpan> spans, bool prior_unknown = false) {
    return HostLease::declare_current(names, spans, prior_unknown);
}
inline void gpu_boundary() {
    if (enabled.load(std::memory_order_acquire)) domain.gpu_boundary();
}
} // namespace switch_validation::epoch
