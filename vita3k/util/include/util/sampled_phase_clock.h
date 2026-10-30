// Bounded, sample-aware ABBA scheduling. Pure integer clock; no wall-clock reads.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <cstdint>
#include <limits>

namespace switch_validation {
class SampledPhaseClock {
public:
    static constexpr uint32_t phase_count = 4, minimum_samples = 50;
    static constexpr uint32_t settle_seconds = 1, minimum_seconds = 6, maximum_seconds = 12;
    struct Step {
        uint32_t phase = 0;
        uint64_t delta = 0;
        bool sample = false, ended = false, capped = false, complete = false;
    };
private:
    uint64_t hz_ = 0, start_ = 0, previous_ = 0, samples_ = 0, measured_ = 0;
    uint32_t index_ = 0;
    bool failed_ = false;
public:
    void reset(uint64_t tick, uint64_t hz) {
        hz_ = hz; start_ = previous_ = tick; samples_ = measured_ = 0; index_ = 0;
        failed_ = !hz || hz > std::numeric_limits<uint64_t>::max() / 100000;
    }
    bool failed() const { return failed_; }
    bool complete() const { return failed_ || index_ == phase_count; }
    uint32_t index() const { return index_; }
    uint64_t samples() const { return samples_; }
    uint64_t measured_ticks() const { return measured_; }
    bool measuring(uint64_t tick) const { return !complete() && tick >= start_ && tick - start_ >= hz_; }
    bool extending(uint64_t tick) const {
        return measuring(tick) && tick - start_ >= uint64_t(settle_seconds + minimum_seconds) * hz_;
    }
    // Conservative remaining upper bound, not a promise of completion at zero.
    uint64_t remaining_ms(uint64_t tick) const {
        if (complete()) return 0;
        const auto span = uint64_t(settle_seconds + maximum_seconds) * hz_;
        const auto elapsed = tick >= start_ ? std::min(tick - start_, span) : 0;
        return ((phase_count - index_ - 1) * span + span - elapsed) * 1000 / hz_;
    }
    uint32_t progress(uint64_t tick) const {
        if (complete()) return failed_ ? 0 : 1000;
        const auto span = uint64_t(settle_seconds + maximum_seconds) * hz_;
        const auto elapsed = tick >= start_ ? std::min(tick - start_, span) : 0;
        return index_ * 250 + static_cast<uint32_t>(std::min<uint64_t>(249, elapsed * 250 / span));
    }
    // Account for time spent publishing the new write table before actual
    // settling begins. The caller passes the actual post-publication tick.
    void defer_start(uint64_t tick) {
        if (!complete() && tick > start_) start_ = previous_ = tick;
    }
    Step advance(uint64_t tick) {
        Step out; out.phase = index_; out.complete = complete();
        if (complete()) return out;
        if (tick < previous_) { failed_ = true; out.complete = true; return out; }
        out.delta = tick - previous_;
        const auto elapsed = tick >= start_ ? tick - start_ : 0;
        const auto limit = uint64_t(settle_seconds + maximum_seconds) * hz_;
        out.sample = out.delta && previous_ >= start_ && previous_ - start_ >= hz_
            && elapsed <= limit;
        if (out.sample) { ++samples_; measured_ += out.delta; }
        previous_ = tick;
        const bool enough = samples_ >= minimum_samples && measured_ >= minimum_seconds * hz_;
        out.capped = elapsed >= limit && !enough;
        out.ended = enough || elapsed >= limit;
        if (out.ended) {
            ++index_; out.complete = index_ == phase_count;
            start_ = previous_ = tick; samples_ = measured_ = 0;
        }
        return out;
    }
};
} // namespace switch_validation
