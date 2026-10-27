// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
// Licensed under GPLv2 or any later version.

#pragma once

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>

namespace renderer::vulkan {

struct DescriptorBindRange {
    uint32_t first_set = 0;
    uint32_t set_count = 0;
};

// Tracks the four graphics sets of one command-buffer recording. Only set 0
// contains dynamic descriptors. Keep the handle types generic so the binding
// decisions can be replayed in host tests without a Vulkan device.
template <typename DescriptorSet, typename PipelineLayout>
class DescriptorBindingCache {
    bool valid = false;
    PipelineLayout layout{};
    std::array<DescriptorSet, 4> sets{};
    std::array<uint32_t, 4> offsets{};
    uint32_t offset_count = 0;

public:
    void invalidate() { valid = false; }

    DescriptorBindRange update(PipelineLayout new_layout,
        const std::array<DescriptorSet, 4> &new_sets,
        const std::array<uint32_t, 4> &new_offsets, uint32_t new_offset_count) {
        assert(new_offset_count <= new_offsets.size());

        uint32_t first = 0;
        uint32_t end = static_cast<uint32_t>(sets.size());
        if (valid && layout == new_layout) {
            first = end;
            end = 0;
            for (uint32_t i = 0; i < sets.size(); ++i) {
                if (sets[i] != new_sets[i]) {
                    first = std::min(first, i);
                    end = i + 1;
                }
            }
            if (offset_count != new_offset_count
                || !std::equal(new_offsets.begin(), new_offsets.begin() + new_offset_count, offsets.begin())) {
                first = 0;
                end = std::max(end, 1U);
            }
            if (end == 0)
                return {};
        }

        // A different layout conservatively rebinds all sets. For an identical
        // layout, rebinding one contiguous changed range cannot disturb the
        // other sets, and avoids multiple driver calls when several change.
        valid = true;
        layout = new_layout;
        sets = new_sets;
        offsets = new_offsets;
        offset_count = new_offset_count;
        return { first, end - first };
    }
};

} // namespace renderer::vulkan
