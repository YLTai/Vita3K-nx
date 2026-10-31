// Exact content comparison, not page-dirty or periodic-Hash reuse.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <list>
#include <new>
#include <unordered_map>
#include <vector>

namespace switch_validation::content {
using Hash = uint64_t (*)(const void *, size_t);
struct Input {
    const void *pixels = nullptr;
    size_t size = 0;
    const void *palette = nullptr;
    size_t palette_size = 0;
};
enum class Mode : uint8_t { Full, Audit, Compare };
inline const char *mode_name(Mode mode) {
    return mode == Mode::Full ? "full_hash" : mode == Mode::Audit ? "memcmp_audit" : "memcmp_exact";
}
inline uint64_t full_digest(const Input &in, Hash hash) {
    return (in.pixels ? hash(in.pixels, in.size) : 0)
        ^ (in.palette_size ? hash(in.palette, in.palette_size) : 0);
}
struct Counts {
    uint64_t calls = 0, input_bytes = 0;
    uint64_t hits = 0, equal_bytes = 0, refreshes = 0, copied_bytes = 0;
    uint64_t fallback_calls = 0, evictions = 0, allocation_failures = 0;
    uint64_t digest_calls = 0, digest_bytes = 0;
    uint64_t audit_checks = 0, audit_disagreements = 0;
};
struct Failure {
    uintptr_t pixels = 0;
    uint64_t size = 0, candidate = 0, reference = 0;
};

// Renderer-thread owned. Never dereference a retained source pointer: keys are
// integer identities only; all comparisons use this call's validated pointers.
// Same-address reuse and unreported CPU/HLE writes are detected by byte equality.
// CPU/GPU visibility, mapping lifetime and racing writers are still the caller's
// responsibility, just as they are for full_digest. This is NOT a synchronization
// mechanism. The hash describes the exact private snapshot we actually copied.
class Memo {
    struct Key {
        uintptr_t pixels, palette;
        size_t size, palette_size;
        bool operator==(const Key &) const = default;
    };
    struct KeyHash {
        size_t operator()(const Key &k) const {
            return (k.pixels >> 4) ^ (k.palette << 1) ^ k.size ^ (k.palette_size << 5);
        }
    };
    struct Entry { Key key; std::vector<uint8_t> bytes; uint64_t digest = 0; };
    using List = std::list<Entry>;
    List lru_;
    std::unordered_map<Key, typename List::iterator, KeyHash> entries_;
    size_t resident_ = 0, peak_ = 0;
    size_t budget_, max_entries_, minimum_;
    bool disabled_ = false;
    Failure failure_{};

    static bool valid_span(const void *p, size_t n) {
        return n == 0 || (p && n <= std::numeric_limits<uintptr_t>::max() - reinterpret_cast<uintptr_t>(p));
    }
    uint64_t digest(const Input &in, Hash hash, Counts &counts) const {
        ++counts.digest_calls;
        counts.digest_bytes += in.size + in.palette_size;
        return full_digest(in, hash);
    }
    void evict(Counts &counts) {
        auto &last = lru_.back();
        resident_ -= last.bytes.size();
        entries_.erase(last.key); lru_.pop_back(); ++counts.evictions;
    }
public:
    explicit Memo(size_t budget = 64 * 1024 * 1024, size_t max_entries = 1024, size_t minimum = 4096)
        : budget_(budget), max_entries_(max_entries), minimum_(minimum) {}
    void clear() { entries_.clear(); lru_.clear(); resident_ = 0; }
    void reset() { clear(); disabled_ = false; failure_ = {}; peak_ = 0; }
    bool disabled() const { return disabled_; }
    size_t resident_bytes() const { return resident_; }
    size_t peak_bytes() const { return peak_; }
    size_t budget_bytes() const { return budget_; }
    const Failure &failure() const { return failure_; }

    uint64_t evaluate(const Input &in, Mode mode, Hash hash, Counts &counts) {
        ++counts.calls;
        const bool size_ok = in.palette_size <= std::numeric_limits<size_t>::max() - in.size;
        const size_t bytes = size_ok ? in.size + in.palette_size : 0;
        counts.input_bytes += bytes;
        // Invalid buffers must already have been rejected by the caller. Do not
        // manufacture a Hash for bad input. Oversized/empty/small resources use
        // exactly the pre-existing full-hash path, without caching anything.
        if (mode == Mode::Full || disabled_ || !size_ok || !in.pixels || !in.size
            || bytes < minimum_ || bytes > budget_ || !max_entries_
            || !valid_span(in.pixels, in.size) || !valid_span(in.palette, in.palette_size)) {
            ++counts.fallback_calls;
            return digest(in, hash, counts);
        }
        const Key key{reinterpret_cast<uintptr_t>(in.pixels), reinterpret_cast<uintptr_t>(in.palette), in.size, in.palette_size};
        uint64_t candidate = 0;
        try {
            auto found = entries_.find(key);
            if (found != entries_.end()) {
                auto it = found->second;
                const bool equal = std::memcmp(in.pixels, it->bytes.data(), in.size) == 0
                    && (!in.palette_size || std::memcmp(in.palette, it->bytes.data() + in.size, in.palette_size) == 0);
                lru_.splice(lru_.begin(), lru_, it);
                if (equal) {
                    ++counts.hits; counts.equal_bytes += bytes;
                    candidate = it->digest;
                } else {
                    std::memcpy(it->bytes.data(), in.pixels, in.size);
                    if (in.palette_size) std::memcpy(it->bytes.data() + in.size, in.palette, in.palette_size);
                    candidate = digest({it->bytes.data(), in.size,
                        in.palette_size ? it->bytes.data() + in.size : nullptr, in.palette_size}, hash, counts);
                    it->digest = candidate; ++counts.refreshes; counts.copied_bytes += bytes;
                }
            } else {
                // Evict BEFORE allocation. Resident byte storage, including the
                // incoming entry, never exceeds budget (metadata is separate).
                while (!lru_.empty() && (resident_ > budget_ - bytes || entries_.size() >= max_entries_)) evict(counts);
                lru_.push_front({key, std::vector<uint8_t>(bytes), 0});
                auto it = lru_.begin();
                std::memcpy(it->bytes.data(), in.pixels, in.size);
                if (in.palette_size) std::memcpy(it->bytes.data() + in.size, in.palette, in.palette_size);
                candidate = digest({it->bytes.data(), in.size,
                    in.palette_size ? it->bytes.data() + in.size : nullptr, in.palette_size}, hash, counts);
                it->digest = candidate;
                entries_.emplace(key, it);
                resident_ += bytes; peak_ = std::max(peak_, resident_);
                ++counts.refreshes; counts.copied_bytes += bytes;
            }
        } catch (const std::bad_alloc &) {
            clear(); disabled_ = true; ++counts.allocation_failures;
            ++counts.fallback_calls;
            return digest(in, hash, counts);
        }
        if (mode == Mode::Audit) {
            ++counts.audit_checks;
            const uint64_t reference = digest(in, hash, counts);
            if (reference != candidate) {
                ++counts.audit_disagreements;
                failure_ = {key.pixels, bytes, candidate, reference};
                clear(); disabled_ = true;
                // Could be a concurrent source change, not necessarily a memo
                // algorithm defect. Fail closed, use the full-source result.
            }
            return reference; // B always renders the independent reference.
        }
        return candidate;
    }
};
} // namespace switch_validation::content
