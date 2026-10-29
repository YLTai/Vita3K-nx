// Logical GXM mappings need not pin host pages until a Vulkan buffer is used.
// No permission changes, content caching, driver calls, or GPU synchronization here.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <mutex>

namespace switch_validation::deferred_import {
enum Use : uint32_t { Buffer = 1, DeviceAddress = 2, CpuSync = 4, Writable = 8, Uniform = 16 };
enum class Phase : uint8_t { Empty, Pending, Creating, Imported, Fallback, Failed, Released };
struct Entry {
    uintptr_t owner = 0, host = 0;
    uint64_t token = 0, bytes = 0, creation_ticks = 0;
    uint32_t guest = 0, uses = 0;
    Phase phase = Phase::Empty, before_release = Phase::Empty;
};
struct Summary {
    uint64_t registered = 0, pending = 0, pending_bytes = 0, imported = 0, imported_bytes = 0;
    uint64_t fallbacks = 0, avoided = 0, avoided_bytes = 0, capacity_fallbacks = 0, errors = 0;
    bool enabled = false, frozen = false;
};
class Registry {
public:
    static constexpr size_t capacity = 1024;
private:
    mutable std::mutex mutex_;
    std::array<Entry, capacity> entries_{}, frozen_entries_{};
    size_t used_ = 0, frozen_used_ = 0;
    uint64_t serial_ = 0, errors_ = 0, capacity_fallbacks_ = 0;
    Summary frozen_summary_{};
    bool frozen_ = false;
    std::atomic<bool> enabled_{false};
    static bool live(const Entry &e) { return e.phase != Phase::Empty && e.phase != Phase::Released; }
    Entry *find(uint64_t token) {
        for (size_t i = 0; i < used_; ++i) if (entries_[i].token == token) return &entries_[i];
        return nullptr;
    }
    Summary summary_unlocked() const {
        Summary s; s.enabled = enabled_.load(); s.registered = used_;
        s.errors = errors_; s.capacity_fallbacks = capacity_fallbacks_;
        for (size_t i = 0; i < used_; ++i) {
            const auto &e = entries_[i];
            if (e.phase == Phase::Pending) { ++s.pending; s.pending_bytes += e.bytes; }
            if (e.phase == Phase::Imported) { ++s.imported; s.imported_bytes += e.bytes; }
            if (e.phase == Phase::Fallback) ++s.fallbacks;
            if (e.phase == Phase::Released && e.before_release == Phase::Pending) {
                ++s.avoided; s.avoided_bytes += e.bytes;
            }
        }
        return s;
    }
public:
    bool enabled() const { return enabled_.load(std::memory_order_relaxed); }
    // Session boundaries only, after old renderer cleanup. Never lose a live lease.
    bool configure(bool enabled) {
        std::lock_guard lock(mutex_);
        for (size_t i = 0; i < used_; ++i) if (live(entries_[i])) { ++errors_; return false; }
        entries_.fill({}); used_ = 0; errors_ = capacity_fallbacks_ = 0;
        frozen_ = false; frozen_used_ = 0; enabled_.store(enabled); return true;
    }
    uint64_t reserve(uintptr_t owner, uint32_t guest, uintptr_t host, uint64_t bytes) {
        std::lock_guard lock(mutex_);
        if (!owner || !guest || !host || !bytes || bytes > (uint64_t{1} << 32) - guest
            || bytes > UINTPTR_MAX - host || ((guest | host | bytes) & 4095)) {
            ++errors_; return 0;
        }
        for (size_t i = 0; i < used_; ++i) if (live(entries_[i]) && entries_[i].owner == owner && entries_[i].guest == guest) {
            ++errors_; return 0; // The renderer handles an identical map idempotently before reserve.
        }
        if (used_ == capacity) { ++capacity_fallbacks_; return 0; } // Caller uses original eager path.
        auto &e = entries_[used_++]; e.owner = owner; e.host = host; e.guest = guest;
        e.bytes = bytes; e.token = ++serial_; e.phase = Phase::Pending; return e.token;
    }
    // A pending registration retains its backing just as an eager driver import did.
    // Called outside the fault handler and while the existing pool lock is held.
    bool blocks_recycle(uintptr_t host, uint64_t bytes) const {
        if (!host || !bytes || bytes > UINTPTR_MAX - host) return true;
        std::lock_guard lock(mutex_);
        for (size_t i = 0; i < used_; ++i) {
            const auto &e = entries_[i];
            if (live(e) && host < e.host + e.bytes && e.host < host + bytes) return true;
        }
        return false;
    }
    Entry get(uint64_t token) {
        std::lock_guard lock(mutex_); auto *e = find(token); return e ? *e : Entry{};
    }
    bool claim(uint64_t token, uintptr_t owner, uintptr_t current_host, uint64_t bytes) {
        std::lock_guard lock(mutex_); auto *e = find(token);
        if (!e || e->phase != Phase::Pending || e->owner != owner || e->host != current_host || e->bytes != bytes) {
            ++errors_; return false;
        }
        e->phase = Phase::Creating; return true;
    }
    bool publish(uint64_t token, bool imported, uint32_t uses, uint64_t ticks) {
        std::lock_guard lock(mutex_); auto *e = find(token);
        if (!e || e->phase != Phase::Creating) { ++errors_; return false; }
        e->phase = imported ? Phase::Imported : Phase::Fallback; e->uses |= uses;
        e->creation_ticks = ticks; return true;
    }
    void note_use(uint64_t token, uint32_t uses) {
        std::lock_guard lock(mutex_); auto *e = find(token);
        if (e && (e->phase == Phase::Imported || e->phase == Phase::Fallback)) e->uses |= uses;
        else ++errors_;
    }
    void fail(uint64_t token) {
        std::lock_guard lock(mutex_); if (auto *e = find(token)) e->phase = Phase::Failed; ++errors_;
    }
    void release(uint64_t token) {
        if (!token) return;
        std::lock_guard lock(mutex_); auto *e = find(token);
        if (!e || !live(*e) || e->phase == Phase::Creating) { ++errors_; return; }
        e->before_release = e->phase; e->phase = Phase::Released;
    }
    void release_owner(uintptr_t owner) {
        std::lock_guard lock(mutex_);
        for (size_t i = 0; i < used_; ++i) if (live(entries_[i]) && entries_[i].owner == owner) {
            if (entries_[i].phase == Phase::Creating) { ++errors_; continue; }
            entries_[i].before_release = entries_[i].phase; entries_[i].phase = Phase::Released;
        }
    }
    void start_window() { std::lock_guard lock(mutex_); frozen_ = false; }
    void freeze_window() {
        std::lock_guard lock(mutex_); if (frozen_) return;
        frozen_summary_ = summary_unlocked(); frozen_summary_.frozen = true;
        frozen_entries_ = entries_; frozen_used_ = used_; frozen_ = true;
    }
    Summary summary() const {
        std::lock_guard lock(mutex_); auto s = frozen_ ? frozen_summary_ : summary_unlocked();
        // A later lifetime fault is never hidden by a clean capture snapshot.
        s.errors = errors_; return s;
    }
    void write_json(FILE *f) const {
        std::lock_guard lock(mutex_);
        const auto s = frozen_ ? frozen_summary_ : summary_unlocked();
        const auto &entries = frozen_ ? frozen_entries_ : entries_;
        const size_t count = frozen_ ? frozen_used_ : used_;
        std::fprintf(f, "{\"enabled\":%s,\"frozen_at_window_end\":%s,\"registered\":%llu,"
            "\"pending\":%llu,\"pending_bytes\":%llu,\"imported\":%llu,\"imported_bytes\":%llu,"
            "\"fallbacks\":%llu,\"released_without_import\":%llu,\"avoided_bytes\":%llu,"
            "\"capacity_fallbacks\":%llu,\"errors\":%llu,\"records\":[",
            s.enabled ? "true" : "false", frozen_ ? "true" : "false",
            (unsigned long long)s.registered, (unsigned long long)s.pending, (unsigned long long)s.pending_bytes,
            (unsigned long long)s.imported, (unsigned long long)s.imported_bytes, (unsigned long long)s.fallbacks,
            (unsigned long long)s.avoided, (unsigned long long)s.avoided_bytes,
            (unsigned long long)s.capacity_fallbacks, (unsigned long long)errors_);
        for (size_t i = 0; i < count; ++i) {
            const auto &e = entries[i];
            std::fprintf(f, "%s{\"id\":%llu,\"guest\":%u,\"host\":%llu,\"bytes\":%llu,"
                "\"phase\":%u,\"before_release\":%u,\"use_mask\":%u,\"creation_ticks\":%llu}",
                i ? "," : "", (unsigned long long)e.token, e.guest, (unsigned long long)e.host,
                (unsigned long long)e.bytes, unsigned(e.phase), unsigned(e.before_release), e.uses,
                (unsigned long long)e.creation_ticks);
        }
        std::fputs("],\"phase_legend\":\"1=pending 2=creating 3=imported 4=fallback 5=failed 6=released\","
            "\"use_legend\":\"1=buffer/transfer 2=device-address 4=CPU-sync 8=possible-shader-write 16=uniform\","
            "\"scope\":\"whole logical mappings; overlapping spans count again; not unique RAM or write coverage\"}", f);
    }
};
inline Registry registry;
} // namespace switch_validation::deferred_import
