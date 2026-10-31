// Concrete host write intervals, including writes with a runtime-known extent.
// No whole-import whitelist, no guessed pcm_size_max, and no content authority.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <util/jit_epoch_gate.h>
#include <climits>
#include <utility>

namespace switch_validation::epoch {
struct WriteExtent {
    uint64_t bytes = 0;
    bool valid = true;
};
inline WriteExtent packed_pcm_extent(int64_t samples, int64_t channels, int64_t width) {
    if (samples < 0 || channels <= 0 || width <= 0) return {0, false};
    const auto s = uint64_t(samples), c = uint64_t(channels), w = uint64_t(width);
    if (c > UINT64_MAX / w || s > UINT64_MAX / (c * w)) return {0, false};
    return {s * c * w, true};
}
// Bound the CURRENT MP3 loop, not an assumed packed layout. Do not silently
// change its existing indexing while testing memory tracking. The formula is
// (width+channels)*i + ch*channels + j, computed as int in the old loop.
inline WriteExtent mp3_loop_extent(int samples, int channels, int width) {
    if (samples <= 0 || channels <= 0 || width <= 0) return {};
    const uint64_t stride = uint64_t(width) + uint64_t(channels);
    const uint64_t row = stride * uint64_t(samples - 1);
    const uint64_t tail = uint64_t(channels - 1) * uint64_t(channels) + uint64_t(width);
    if (stride > INT_MAX || row > INT_MAX || tail > INT_MAX || row + tail - 1 > INT_MAX)
        return {0, false};
    return {row + tail, true};
}
enum class OutputSource : unsigned { Generic = 0, Atrac9 = 1, Aac = 2, Mp3 = 3, GxmUniform = 4 };
class HostOutputLease {
    Domain *domain_ = nullptr;
    unsigned slot_ = HleRanges::no_slot;
public:
    HostOutputLease(void *destination, WriteExtent extent, OutputSource source = OutputSource::Generic,
        Domain *d = enabled.load(std::memory_order_acquire) ? &domain : nullptr) : domain_(d) {
        if (domain_) slot_ = domain_->begin_output({destination, extent.bytes}, unsigned(source),
            extent.valid && (!extent.bytes || destination));
    }
    ~HostOutputLease() { if (domain_ && slot_ != HleRanges::no_slot) domain_->end_output(slot_); }
    HostOutputLease(const HostOutputLease &) = delete;
    HostOutputLease &operator=(const HostOutputLease &) = delete;
    HostOutputLease(HostOutputLease &&other) noexcept
        : domain_(std::exchange(other.domain_, nullptr)), slot_(std::exchange(other.slot_, HleRanges::no_slot)) {}
    HostOutputLease &operator=(HostOutputLease &&) = delete;
};
} // namespace switch_validation::epoch
