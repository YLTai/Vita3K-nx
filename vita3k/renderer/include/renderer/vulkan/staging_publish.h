// Cache publication policy for the Switch staging A/B validation experiment.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

namespace renderer::vulkan {

struct StagingPublishPlan {
    bool platform_flush;
    bool vulkan_flush;
    bool deduplicated;
};

// The pinned Switch Mesa driver publishes non-coherent mapped allocations
// through vkFlushMappedMemoryRanges -> armDCacheClean. Its reported coherent
// mappings retain the existing explicit platform-flush workaround. This policy
// applies only to VMA-owned staging memory, never imported Guest host pointers.
constexpr StagingPublishPlan staging_publish_plan(bool is_coherent, bool deduplicate_requested) {
    if (is_coherent)
        return { true, false, false };
    if (deduplicate_requested)
        return { false, true, true };
    return { true, true, false };
}

} // namespace renderer::vulkan
