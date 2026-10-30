// Resource-scoped, generation-based content-hash reuse.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
#include <type_traits>
#include <utility>

namespace switch_validation::reuse {

// No diagnostic candidate count, equal previous hash, sampling cadence or time
// interval can manufacture this coverage. The source adapter must certify all
// five writer/lifetime paths for THIS allocation before permitting reuse.
enum Coverage : uint32_t {
    CpuStores = 1u << 0, SynchronousHost = 1u << 1,
    AsynchronousHost = 1u << 2, GpuWrites = 1u << 3, MappingLifetime = 1u << 4,
    CompleteCoverage = CpuStores | SynchronousHost | AsynchronousHost | GpuWrites | MappingLifetime
};
enum class Mode : uint8_t { Reference, Audit, Performance };
enum class Reason : uint8_t {
    None, InvalidInput, Unsupported, Busy, StaleLifetime, Cold, VersionChanged,
    NotAudited, ConcurrentChange, Capacity, Disabled, Count
};

struct Input {
    uintptr_t pixels = 0, palette = 0;
    uint64_t pixel_bytes = 0, palette_bytes = 0;
    // Distinguishes hash algorithms/seed and formats whose digest composition
    // is not the ordinary XXH3(pixel) XOR XXH3(palette) definition.
    uint64_t hash_domain = 0;
    bool operator==(const Input &) const = default;
    bool valid() const noexcept {
        const auto span = [](uintptr_t p, uint64_t n) {
            return (!n && !p) || (p && n && n <= std::numeric_limits<uintptr_t>::max() - p);
        };
        return pixel_bytes && span(pixels, pixel_bytes) && span(palette, palette_bytes)
            && palette_bytes <= std::numeric_limits<uint64_t>::max() - pixel_bytes;
    }
    uint64_t bytes() const noexcept { return pixel_bytes + palette_bytes; }
};

// Immutable observation of an allocation-owned input. Snapshot acquisition is
// the linearization point for a reuse decision, just as completing a reference
// hash is the original cache's observation point. It is NOT a lock on later GPU
// upload: the renderer's existing upload/ownership protocol remains mandatory.
struct Evidence {
    uint64_t allocation = 0, lifetime = 0, mapping = 0, publication = 0;
    uint64_t cpu = 0, host = 0, async = 0, gpu = 0, unknown = 0;
    uint32_t coverage = 0;
    bool valid = false, publication_drained = false;
    bool host_busy = false, async_busy = false, gpu_busy = false, unknown_busy = false;
    // Set only after the existing required completion AND CPU visibility work.
    // Equality of GPU submission counters cannot set this field.
    bool gpu_host_visible = false;
    // Keep independent writer histories separate: XOR/summing version numbers
    // can accidentally make two distinct histories look equal.
    uint64_t host_notice = 0, external_host = 0, external_lifetime = 0;

    Reason rejection() const noexcept {
        if (!valid || !allocation || !lifetime || !mapping || !publication)
            return Reason::StaleLifetime;
        if ((coverage & CompleteCoverage) != CompleteCoverage)
            return Reason::Unsupported;
        if (!publication_drained || host_busy || async_busy || gpu_busy || unknown_busy || !gpu_host_visible)
            return Reason::Busy;
        return Reason::None;
    }
    bool operator==(const Evidence &) const = default;
};
struct Counters {
    uint64_t calls = 0, reference_calls = 0, reference_bytes = 0;
    uint64_t reused_calls = 0, reused_bytes = 0;
    uint64_t audits = 0, audit_bytes = 0, mismatches = 0, raced_hashes = 0;
    uint64_t exceptions = 0;
    std::array<uint64_t, size_t(Reason::Count)> fallback{};
};
struct Counterexample {
    Input input{};
    Evidence evidence{};
    uint64_t cached = 0, reference = 0, phase = 0;
};
struct Report {
    bool enabled = false, failed = false;
    uint32_t phase = 0;
    Mode mode = Mode::Reference;
    Counters total{};
    std::array<Counters, 6> phases{};
    uint64_t examples = 0, examples_dropped = 0;
    std::array<Counterexample, 16> first_examples{};
};

// Single renderer consumer, arbitrary producer concurrency via SnapshotFn.
// Fixed storage; no allocation or content copy in lookup. The internal mutex
// protects metadata only; no caller code or hash runs while it is held.
// A source-adapter contract is a prerequisite, not something tests can enable
// merely by observing many equal hashes. Reference mode establishes baselines;
// Audit always returns the full reference; Performance requires that the same
// allocation/input has actually passed an Audit comparison in this session.
template<size_t Capacity = 1024> class HashCache {
    static_assert(Capacity > 0 && (Capacity & (Capacity - 1)) == 0);
    struct Entry {
        Input input{};
        Evidence evidence{};
        uint64_t hash = 0;
        bool used = false, audited = false;
    };
    std::array<Entry, Capacity> entries_{};
    mutable std::mutex mutex_;
    Report report_{};
    uint64_t session_ = 1, phase_serial_ = 1;

    static size_t bucket(const Input &in) noexcept {
        // Exact identity is checked on hits; this is not a content hash.
        auto x = uint64_t(in.pixels) ^ (uint64_t(in.palette) << 1) ^ in.pixel_bytes
            ^ (in.palette_bytes << 17) ^ (in.hash_domain << 7);
        x ^= x >> 33; x *= 0xff51afd7ed558ccdULL; x ^= x >> 33;
        return size_t(x) & (Capacity - 1);
    }
    template<class F> void count(F &&fn) noexcept {
        fn(report_.total);
        if (report_.phase < report_.phases.size()) fn(report_.phase < report_.phases.size() ? report_.phases[report_.phase] : report_.total);
    }
    void fallback(Reason reason) noexcept {
        if (reason != Reason::None) count([&](Counters &c) { ++c.fallback[size_t(reason)]; });
    }
    void failure(const Input &input, const Evidence &proof, uint64_t old_hash, uint64_t hash) noexcept {
        report_.failed = true;
        report_.enabled = false;
        count([](Counters &c) { ++c.mismatches; });
        if (report_.examples < report_.first_examples.size())
            report_.first_examples[report_.examples++] = {input, proof, old_hash, hash, report_.phase};
        else ++report_.examples_dropped;
        for (auto &e : entries_) e = {};
    }
public:
    void begin() {
        std::lock_guard lock(mutex_);
        // A new session is only legal with the previous renderer consumer idle.
        // Generation exhaustion fails closed instead of making old proof current.
        if (session_ == UINT64_MAX || phase_serial_ == UINT64_MAX) { report_.failed = true; report_.enabled = false; return; }
        ++session_; ++phase_serial_;
        entries_.fill({}); report_ = {}; report_.enabled = true;
    }
    void phase(uint32_t phase, Mode mode) {
        std::lock_guard lock(mutex_);
        ++phase_serial_;
        if (phase >= report_.phases.size() || !phase_serial_) {
            report_.failed = true; report_.enabled = false; return;
        }
        report_.phase = phase; report_.mode = mode;
        // Do not clear source histories or audit qualification at phase changes.
    }
    void stop() { std::lock_guard lock(mutex_); report_.enabled = false; ++phase_serial_; }
    bool failed() const { std::lock_guard lock(mutex_); return report_.failed; }
    Report report() const { std::lock_guard lock(mutex_); return report_; }

    template<class SnapshotFn, class HashFn>
    uint64_t evaluate(const Input &input, SnapshotFn &&snapshot, HashFn &&full_hash) {
        static_assert(std::is_nothrow_invocable_r_v<Evidence, SnapshotFn>,
            "source snapshots must be noexcept; invalid evidence is fallback");
        const auto before = snapshot();
        uint64_t session = 0, phase_serial = 0, expected = 0;
        bool candidate = false, inherited_audit = false;
        size_t slot = 0;
        Reason why = Reason::None;
        Mode mode;
        {
            std::lock_guard lock(mutex_);
            session = session_; phase_serial = phase_serial_; mode = report_.mode;
            count([](Counters &c) { ++c.calls; });
            if (!report_.enabled || report_.failed) why = Reason::Disabled;
            else if (!input.valid()) why = Reason::InvalidInput;
            else why = before.rejection();
            if (why == Reason::None) {
                slot = bucket(input);
                auto &e = entries_[slot];
                const bool identity = e.used && e.input == input
                    && e.evidence.allocation == before.allocation
                    && e.evidence.lifetime == before.lifetime
                    && e.evidence.mapping == before.mapping;
                inherited_audit = identity && e.audited;
                candidate = identity && e.evidence == before;
                if (candidate) expected = e.hash;
                else why = !e.used ? Reason::Cold : identity ? Reason::VersionChanged : Reason::Capacity;
                if (candidate && mode == Mode::Performance && !e.audited) {
                    candidate = false; why = Reason::NotAudited;
                }
            }
            if (why != Reason::None && why != Reason::Cold && why != Reason::VersionChanged
                && why != Reason::NotAudited && why != Reason::Capacity && input.valid()) {
                // A withdrawn coverage contract must not become reusable again
                // just by restoring its old bits. Discard its audit and baseline
                // even if the source adapter forgot to advance an epoch.
                auto &old = entries_[bucket(input)];
                if (old.used && old.input == input) {
                    const bool same_live_input = old.evidence.allocation == before.allocation
                        && old.evidence.lifetime == before.lifetime && old.evidence.mapping == before.mapping;
                    // A tracked in-flight operation revokes the hash baseline,
                    // not the input's prior independent audit. After it ends,
                    // one stable full Hash is mandatory before fast reuse.
                    // Withdrawn coverage/lifetime still discards everything.
                    if (why == Reason::Busy && same_live_input
                        && (before.coverage & CompleteCoverage) == CompleteCoverage)
                        old.evidence.publication = 0;
                    else old = {};
                }
            }
            if (mode == Mode::Performance && candidate) {
                // No raw bytes read, no memcpy, no HashFn invocation.
                count([&](Counters &c) { ++c.reused_calls; c.reused_bytes += input.bytes(); });
                return expected;
            }
            fallback(why);
            count([&](Counters &c) { ++c.reference_calls;
                if (input.valid()) c.reference_bytes += input.bytes(); });
        }
        uint64_t actual = 0;
        try { actual = full_hash(); }
        catch (...) {
            std::lock_guard lock(mutex_);
            if (session == session_) {
                report_.failed = true; report_.enabled = false;
                count([](Counters &c) { ++c.exceptions; });
                entries_.fill({});
            }
            throw; // Never replace an actual read/Hash failure with stale content.
        }
        const auto after = snapshot();
        {
            std::lock_guard lock(mutex_);
            if (session != session_ || phase_serial != phase_serial_ || !report_.enabled || report_.failed)
                return actual;
            if (!input.valid() || before.rejection() != Reason::None || after.rejection() != Reason::None)
                return actual;
            if (!(before == after)) {
                count([](Counters &c) { ++c.raced_hashes; });
                fallback(Reason::ConcurrentChange);
                auto &old = entries_[bucket(input)];
                if (old.used && old.input == input) old = {};
                return actual; // Do not seed a baseline from a raced reference.
            }
            bool audited = inherited_audit;
            if (mode == Mode::Audit && candidate) {
                count([&](Counters &c) { ++c.audits; c.audit_bytes += input.bytes(); });
                if (expected != actual) { failure(input, after, expected, actual); return actual; }
                audited = true;
            }
            entries_[slot] = {input, after, actual, true, audited};
        }
        return actual;
    }
};

// Fixed six-stage integrated run, replacing isolated count/whitelist rounds.
// The caller feeds real rendered intervals only. Pauses, transitions and
// load/scene changes must not be submitted as measured intervals.
struct Plan {
    static constexpr std::array<Mode, 6> modes{Mode::Reference, Mode::Audit,
        Mode::Performance, Mode::Performance, Mode::Audit, Mode::Reference};
    uint32_t phase = 0, samples = 0;
    uint64_t elapsed_ns = 0;
    bool done = false, failed = false;
    void interval(uint64_t ns, bool valid) noexcept {
        if (done || failed || !valid || !ns) return;
        if (ns > UINT64_MAX - elapsed_ns) { failed = done = true; return; }
        elapsed_ns += ns; ++samples;
    }
    bool ready_to_advance() const noexcept {
        return !done && !failed && ((elapsed_ns >= 4'000'000'000ULL && samples >= 32)
            || elapsed_ns >= 10'000'000'000ULL);
    }
    bool advance() noexcept {
        if (!ready_to_advance()) return false;
        if (++phase == modes.size()) done = true;
        samples = 0; elapsed_ns = 0; return true;
    }
};
} // namespace switch_validation::reuse
