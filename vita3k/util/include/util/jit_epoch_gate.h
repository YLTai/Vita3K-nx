// Non-blocking grace periods for JIT page-table publication. No store instrumentation.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <limits>
#include <mutex>

namespace switch_validation::epoch {
struct Stamp {
    uint64_t ticket = 0, external = 0, hle_active = 0;
    bool drained = false, valid = false;
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
public:
    static constexpr unsigned invalid_slot = 64;
    bool reset() {
        std::lock_guard lock(mutex_);
        if (stats_.active || stats_.host_active) { stats_.failed = true; return false; }
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
    Stamp stamp(uint64_t ticket) const {
        std::lock_guard lock(mutex_);
        bool drained = ticket && ticket <= generation_ && !stats_.failed;
        // Once a ticket is drained, no future entrant can predate it. Avoid
        // scanning 64 slots on every unchanged texture Hash after that point.
        if (drained && ticket > drained_through_) {
            for (uint64_t reader : readers_) if (reader && reader < ticket) drained = false;
            if (drained) drained_through_ = ticket;
        }
        return {ticket, external_, stats_.host_active, drained, !stats_.failed};
    }
    void host_enter() {
        std::lock_guard lock(mutex_);
        ++stats_.host_entries; ++stats_.host_active; advance_external();
    }
    void host_leave() {
        std::lock_guard lock(mutex_);
        if (!stats_.host_active) { stats_.failed = true; return; }
        ++stats_.host_exits; --stats_.host_active; advance_external();
    }
    void gpu_boundary() {
        std::lock_guard lock(mutex_);
        ++stats_.gpu_boundaries; advance_external();
    }
    Stats snapshot() const { std::lock_guard lock(mutex_); return stats_; }
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
class HostLease {
    Domain *domain_ = nullptr;
public:
    explicit HostLease(Domain *d = enabled.load(std::memory_order_acquire) ? &domain : nullptr) : domain_(d) {
        if (domain_) domain_->host_enter();
    }
    ~HostLease() { if (domain_) domain_->host_leave(); }
    HostLease(const HostLease &) = delete;
    HostLease &operator=(const HostLease &) = delete;
};
inline void gpu_boundary() {
    if (enabled.load(std::memory_order_acquire)) domain.gpu_boundary();
}
} // namespace switch_validation::epoch
