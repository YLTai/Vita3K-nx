// Live first-write observation, NEVER a texture-validity authority.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <util/switch_write_watch_api.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace switch_validation::watch {
using Page = uint8_t *;
constexpr uint32_t page_size = 4096;
constexpr uint32_t page_count = 1u << 20;
constexpr uint32_t no_index = ~0u;
struct Counts {
    uint64_t hash_checks = 0, hash_bytes = 0, covered_checks = 0;
    uint64_t changed = 0, changed_cpu_seen = 0, changed_unobserved = 0, changed_uncovered = 0;
    uint64_t callback_stores = 0, callback_reads = 0, exclusive_success = 0;
    uint64_t invalid_accesses = 0, first_write_pages = 0, armed_pages = 0, armed_aliases = 0;
    uint64_t mapping_updates = 0, resources_rejected = 0;
};
struct Event {
    enum Kind : uint32_t { FirstWrite, ChangedUnobserved, MappingChange, InvalidAccess };
    Kind kind = FirstWrite;
    uint32_t phase = no_index, address = 0, size = 0, resource = no_index;
    uint64_t tick = 0, epoch = 0, before_hash = 0, after_hash = 0;
};
struct Key {
    uintptr_t pixels = 0, palette = 0;
    uint64_t size = 0, palette_size = 0;
    bool operator==(const Key &) const = default;
};
struct KeyHash {
    size_t operator()(const Key &k) const {
        // This indexes exact keys; collisions are resolved by operator==.
        return std::hash<uintptr_t>{}(k.pixels) ^ (std::hash<uintptr_t>{}(k.palette) << 1)
            ^ (std::hash<uint64_t>{}(k.size) << 2) ^ (k.palette_size << 3);
    }
};
struct Snapshot {
    bool bound = false, failed = false, observing = false, capacity_limited = false;
    uint64_t epoch = 0, event_count = 0, events_dropped = 0;
    uint64_t resources = 0, backing_pages = 0, late_resources = 0;
    std::array<Counts, 8> phases{};
    std::array<Event, 128> events{};
};

// Mutating mappings, callbacks, and metadata share this mutex. Native JIT
// accesses do NOT take it. The mapping owner must hold hold() across unmap /
// external-copy lifetime changes, not just across the final PTE publication.
// Arming does not revoke a PTE already loaded by a JIT. Observations therefore
// cannot justify Hash skipping or prove absence of CPU/HLE/GPU writes.
class Table {
    struct Group {
        uintptr_t host = 0;
        uint64_t epoch = 0;
        bool armed = false, cpu_seen = false;
        std::vector<uint32_t> aliases, resources;
    };
    struct Resource {
        Key key;
        uint32_t address = 0;
        uint64_t hash = 0;
        bool baseline = false, covered = false, cpu_seen = false;
        std::vector<uint32_t> groups;
    };
    mutable std::recursive_mutex mutex_;
    Page *raw_ = nullptr;
    std::unique_ptr<Page[]> writes_;
    std::unordered_map<uintptr_t, uint32_t> by_host_;
    std::unordered_map<Key, uint32_t, KeyHash> by_key_;
    std::vector<Group> groups_;
    std::vector<Resource> resources_;
    Snapshot stats_;
    uint32_t phase_ = no_index;
    std::array<uint32_t, 4> event_kinds_{};
    bool collecting_ = false;
    size_t edges_ = 0;
    uint64_t (*clock_)() = nullptr;
    static constexpr size_t max_resources = 1024, max_groups = 16384, max_edges = 131072;

    Counts &count() { return stats_.phases[phase_ < 8 ? phase_ : 0]; }
    static uintptr_t host_page(Page entry, uint32_t page) {
        return entry ? reinterpret_cast<uintptr_t>(entry) + uint64_t(page) * page_size : 0;
    }
    static Page load(Page *table, uint32_t page) { return __atomic_load_n(table + page, __ATOMIC_RELAXED); }
    static void store(Page *table, uint32_t page, Page value) { __atomic_store_n(table + page, value, __ATOMIC_RELEASE); }
    void event(Event e) {
        // Reserve room for unexplained changes instead of letting an early
        // burst of first-write/mapping events consume the entire event buffer.
        if (e.kind != Event::ChangedUnobserved && event_kinds_[e.kind] >= 32) {
            ++stats_.events_dropped; return;
        }
        ++event_kinds_[e.kind];
        e.phase = phase_; e.epoch = stats_.epoch; e.tick = clock_ ? clock_() : 0;
        if (stats_.event_count < stats_.events.size()) stats_.events[stats_.event_count++] = e;
        else ++stats_.events_dropped;
    }
    void open_all() {
        if (!writes_ || !raw_) return;
        for (auto &g : groups_) {
            for (uint32_t p : g.aliases) store(writes_.get(), p, load(raw_, p));
            g.armed = false;
        }
        stats_.observing = false;
    }
    void fail() noexcept {
        // No exception or incomplete registration may strand a NULL entry.
        stats_.failed = true; collecting_ = false; stats_.observing = false;
        if (writes_ && raw_)
            for (uint32_t p = 0; p < page_count; ++p) store(writes_.get(), p, load(raw_, p));
    }
    bool valid(uint32_t address, size_t size) const {
        if (address < page_size || !size || size > (uint64_t{1} << 32) - address || !raw_) return false;
        for (uint64_t p = address / page_size; p <= (uint64_t(address) + size - 1) / page_size; ++p)
            if (!load(raw_, static_cast<uint32_t>(p))) return false;
        return true;
    }
    void copy(uint32_t address, void *buffer, size_t size, bool write) {
        auto *bytes = static_cast<uint8_t *>(buffer);
        for (size_t done = 0; done < size;) {
            const auto at = static_cast<uint32_t>(uint64_t(address) + done);
            size_t n = std::min(size - done, size_t(page_size - (at & (page_size - 1))));
            auto *host = reinterpret_cast<uint8_t *>(reinterpret_cast<uintptr_t>(load(raw_, at / page_size)) + at);
            if (write) std::memcpy(host, bytes + done, n);
            else std::memcpy(bytes + done, host, n);
            done += n;
        }
    }
    void written(uint32_t address, size_t size) {
        if (!stats_.observing) return;
        for (uint64_t p = address / page_size; p <= (uint64_t(address) + size - 1) / page_size; ++p) {
            const auto found = by_host_.find(host_page(load(raw_, static_cast<uint32_t>(p)), static_cast<uint32_t>(p)));
            if (found == by_host_.end()) continue;
            auto &g = groups_[found->second];
            if (g.epoch != stats_.epoch || !g.armed) continue;
            g.cpu_seen = true;
            for (uint32_t r : g.resources) resources_[r].cpu_seen = true;
            ++count().first_write_pages;
            event({Event::FirstWrite, no_index, address, static_cast<uint32_t>(size)});
            // Publish only after the actual successful store. All software
            // aliases of this host backing reopen together.
            for (uint32_t alias : g.aliases) store(writes_.get(), alias, load(raw_, alias));
            g.armed = false;
        }
    }
    bool add_span(Resource &r, uintptr_t begin, uint64_t size) {
        if (!size) return true;
        if (!begin || size > std::numeric_limits<uintptr_t>::max() - begin) return false;
        const uintptr_t end = (begin + size - 1) & ~(uintptr_t(page_size) - 1);
        for (uintptr_t p = begin & ~(uintptr_t(page_size) - 1);; p += page_size) {
            auto it = by_host_.find(p);
            if (it == by_host_.end()) {
                if (groups_.size() >= max_groups) return false;
                const uint32_t id = static_cast<uint32_t>(groups_.size());
                groups_.push_back(Group{p, 0, false, false, {}, {}});
                it = by_host_.emplace(p, id).first;
            }
            r.groups.push_back(it->second);
            if (p == end) break;
        }
        return true;
    }
public:
    auto hold() { return std::unique_lock<std::recursive_mutex>(mutex_); }
    void set_clock(uint64_t (*fn)()) { auto lock = hold(); clock_ = fn; }
    Page *bind(Page *raw) noexcept {
        auto lock = hold();
        if (!raw || stats_.failed) return nullptr;
        if (raw_) return raw == raw_ ? writes_.get() : nullptr;
        try {
            auto table = std::make_unique<Page[]>(page_count);
            for (uint32_t p = 0; p < page_count; ++p) table[p] = load(raw, p);
            groups_.reserve(max_groups); resources_.reserve(max_resources);
            by_host_.reserve(max_groups); by_key_.reserve(max_resources);
            raw_ = raw; writes_ = std::move(table); stats_.bound = true;
            return writes_.get();
        } catch (...) { stats_.failed = true; return nullptr; }
    }
    // Call only after all JIT/renderer workers have joined.
    void reset() {
        auto lock = hold();
        raw_ = nullptr; writes_.reset(); by_host_.clear(); by_key_.clear();
        groups_.clear(); resources_.clear(); stats_ = {}; phase_ = no_index; event_kinds_ = {};
        collecting_ = false; edges_ = 0;
    }
    void detach(Page *raw) {
        auto lock = hold();
        if (raw_ != raw) return;
        open_all(); raw_ = nullptr; collecting_ = false; // Retain JIT table until session teardown.
    }
    void publish(Page *raw, uint32_t page, Page entry) noexcept {
        auto lock = hold();
        if (!raw || page >= page_count) return;
        const Page old = load(raw, page);
        store(raw, page, entry);
        if (raw != raw_ || !writes_) return;
        store(writes_.get(), page, entry);
        if (phase_ < 8) ++count().mapping_updates;
        if (old == entry || stats_.failed) return;
        try {
            if (const auto it = by_host_.find(host_page(old, page)); it != by_host_.end()) {
                auto &g = groups_[it->second];
                std::erase(g.aliases, page);
                for (uint32_t r : g.resources) {
                    resources_[r].baseline = false;
                    resources_[r].covered = false;
                }
                if (stats_.observing) event({Event::MappingChange, no_index, page * page_size, page_size});
            }
            if (const auto it = by_host_.find(host_page(entry, page)); entry && it != by_host_.end()) {
                auto &g = groups_[it->second];
                if (std::find(g.aliases.begin(), g.aliases.end(), page) == g.aliases.end()) g.aliases.push_back(page);
                for (uint32_t r : g.resources) { resources_[r].baseline = false; resources_[r].covered = false; }
                if (g.armed && g.epoch == stats_.epoch) store(writes_.get(), page, nullptr);
            }
        } catch (...) { fail(); }
    }
    // A collects footprints; B resolves aliases once, then arms pages. New
    // footprints first seen during B remain explicitly uncovered until a later B.
    void phase(uint32_t phase, bool observe, bool restart = false) noexcept {
        auto lock = hold();
        open_all(); phase_ = phase;
        if (restart) {
            by_host_.clear(); by_key_.clear(); groups_.clear(); resources_.clear(); edges_ = 0;
            const bool bound = stats_.bound, failed = stats_.failed;
            stats_ = {}; event_kinds_ = {}; stats_.bound = bound; stats_.failed = failed;
        }
        collecting_ = phase < 8 && raw_ && !stats_.failed;
        if (!collecting_ || !observe) return;
        try {
            ++stats_.epoch;
            for (auto &g : groups_) { g.aliases.clear(); g.cpu_seen = false; g.epoch = 0; }
            for (uint32_t p = 1; p < page_count; ++p) {
                const Page entry = load(raw_, p);
                if (!entry) continue;
                const auto it = by_host_.find(host_page(entry, p));
                if (it != by_host_.end()) groups_[it->second].aliases.push_back(p);
            }
            for (auto &g : groups_) {
                if (g.aliases.empty()) continue;
                g.epoch = stats_.epoch; g.armed = true;
                ++count().armed_pages; count().armed_aliases += g.aliases.size();
                for (uint32_t p : g.aliases) store(writes_.get(), p, nullptr);
            }
            for (auto &r : resources_) {
                r.baseline = false; r.cpu_seen = false;
                r.covered = std::all_of(r.groups.begin(), r.groups.end(), [&](uint32_t g) { return groups_[g].epoch == stats_.epoch; });
            }
            stats_.observing = true;
        } catch (...) { fail(); }
    }
    Token before(Page *raw, uint32_t address, const void *pixels, uint64_t size,
        const void *palette = nullptr, uint64_t palette_size = 0) noexcept {
        auto lock = hold();
        if (!collecting_ || raw != raw_ || !pixels || !size || address < page_size) return {};
        const Key key{reinterpret_cast<uintptr_t>(pixels), reinterpret_cast<uintptr_t>(palette), size, palette_size};
        try {
            auto it = by_key_.find(key);
            if (it == by_key_.end()) {
                if (resources_.size() >= max_resources || stats_.capacity_limited) { ++count().resources_rejected; return {}; }
                const auto old_groups = groups_.size();
                const auto reject = [&]() -> Token {
                    while (groups_.size() > old_groups) { by_host_.erase(groups_.back().host); groups_.pop_back(); }
                    stats_.capacity_limited = true; ++count().resources_rejected; return {};
                };
                Resource r; r.key = key; r.address = address;
                if (!add_span(r, key.pixels, size) || !add_span(r, key.palette, palette_size)) {
                    return reject();
                }
                std::sort(r.groups.begin(), r.groups.end());
                r.groups.erase(std::unique(r.groups.begin(), r.groups.end()), r.groups.end());
                if (edges_ + r.groups.size() > max_edges) return reject();
                r.covered = stats_.observing && std::all_of(r.groups.begin(), r.groups.end(), [&](uint32_t g) { return groups_[g].epoch == stats_.epoch; });
                r.cpu_seen = stats_.observing && std::any_of(r.groups.begin(), r.groups.end(), [&](uint32_t g) { return groups_[g].epoch == stats_.epoch && groups_[g].cpu_seen; });
                if (stats_.observing && !r.covered) ++stats_.late_resources;
                uint32_t id = static_cast<uint32_t>(resources_.size());
                edges_ += r.groups.size();
                resources_.push_back(std::move(r));
                for (uint32_t g : resources_.back().groups) groups_[g].resources.push_back(id);
                it = by_key_.emplace(key, id).first;
            }
            return {stats_.epoch, it->second};
        } catch (...) { fail(); return {}; }
    }
    void after(Token token, uint64_t hash) noexcept {
        auto lock = hold();
        if (!collecting_ || token.resource >= resources_.size() || token.epoch != stats_.epoch) return;
        auto &r = resources_[token.resource];
        ++count().hash_checks; count().hash_bytes += r.key.size + r.key.palette_size;
        if (stats_.observing && r.covered) ++count().covered_checks;
        if (r.baseline && r.hash != hash) {
            ++count().changed;
            if (!stats_.observing || !r.covered) ++count().changed_uncovered;
            else if (r.cpu_seen) ++count().changed_cpu_seen;
            else {
                ++count().changed_unobserved;
                event({Event::ChangedUnobserved, no_index, r.address,
                    static_cast<uint32_t>(std::min<uint64_t>(r.key.size, UINT32_MAX)), token.resource,
                    0, 0, r.hash, hash});
            }
        }
        r.hash = hash; r.baseline = true;
    }
    Access read(Page *raw, uint32_t address, void *dst, size_t size) {
        auto lock = hold();
        if (raw != raw_ || !writes_) return Access::Unhandled;
        if (phase_ < 8) ++count().callback_reads;
        if (!valid(address, size)) {
            if (phase_ < 8) { ++count().invalid_accesses; event({Event::InvalidAccess, no_index, address, static_cast<uint32_t>(size)}); }
            return Access::Invalid;
        }
        copy(address, dst, size, false); return Access::Complete;
    }
    Access write(Page *raw, uint32_t address, const void *src, size_t size) {
        auto lock = hold();
        if (raw != raw_ || !writes_) return Access::Unhandled;
        if (phase_ < 8) ++count().callback_stores;
        if (!valid(address, size)) {
            if (phase_ < 8) { ++count().invalid_accesses; event({Event::InvalidAccess, no_index, address, static_cast<uint32_t>(size)}); }
            return Access::Invalid;
        }
        copy(address, const_cast<void *>(src), size, true); written(address, size); return Access::Complete;
    }
    template<class T> Access exclusive(Page *raw, uint32_t address, T value, T expected, bool &success) {
        auto lock = hold(); success = false;
        if (raw != raw_ || !writes_) return Access::Unhandled;
        if (!valid(address, sizeof(T)) || address % alignof(T)) {
            if (phase_ < 8) ++count().invalid_accesses;
            return Access::Invalid;
        }
        auto *host = reinterpret_cast<T *>(reinterpret_cast<uintptr_t>(load(raw_, address / page_size)) + address);
        success = __sync_bool_compare_and_swap(host, expected, value);
        if (success) {
            if (phase_ < 8) ++count().exclusive_success;
            written(address, sizeof(T));
        }
        return Access::Complete;
    }
    struct Footprint { uint32_t id, address; Key key; };
    std::vector<Footprint> footprints() const {
        std::lock_guard lock(mutex_);
        std::vector<Footprint> out; out.reserve(resources_.size());
        for (uint32_t i = 0; i < resources_.size(); ++i) out.push_back({i, resources_[i].address, resources_[i].key});
        return out;
    }
    Snapshot snapshot() const {
        std::lock_guard lock(mutex_);
        auto result = stats_; result.resources = resources_.size(); result.backing_pages = groups_.size();
        return result;
    }
};
} // namespace switch_validation::watch
