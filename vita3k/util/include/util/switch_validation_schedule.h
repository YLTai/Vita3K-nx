// Pure scheduling rules shared by the device suite and host regression tests.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <cstdint>

namespace switch_validation {

inline constexpr std::array<bool, 8> staging_dedup_order{
    false, true, true, false, true, false, false, true
};

struct PhasePosition {
    uint32_t index = 0;
    bool complete = false;
    bool measuring = false;
    bool deduplicate = false;
};

constexpr PhasePosition phase_position(uint64_t elapsed_ticks, uint64_t settle_ticks,
    uint64_t measure_ticks) {
    const uint64_t duration = settle_ticks + measure_ticks;
    if (!duration || !measure_ticks)
        return { 0, true, false, false };
    const uint64_t index = elapsed_ticks / duration;
    if (index >= staging_dedup_order.size())
        return { static_cast<uint32_t>(staging_dedup_order.size()), true, false, false };
    return { static_cast<uint32_t>(index), false,
        elapsed_ticks % duration >= settle_ticks,
        staging_dedup_order[index] };
}

// Both ends of an interval must belong to the same measured phase. Never
// assign a warm-up, transition, restart, or pause interval to either policy.
constexpr bool measured_interval(uint64_t begin, uint64_t end,
    uint64_t settle, uint64_t measure) {
    if (end <= begin)
        return false;
    const auto a = phase_position(begin, settle, measure);
    const auto b = phase_position(end, settle, measure);
    return !a.complete && !b.complete && a.measuring && b.measuring && a.index == b.index;
}

} // namespace switch_validation
