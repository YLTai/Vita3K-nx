// No Switch SDK or Vulkan loader required.
// SPDX-License-Identifier: GPL-2.0-or-later
#include <renderer/vulkan/staging_publish.h>

#include <array>
#include <cassert>
#include <cstdint>

using renderer::vulkan::StagingPublishPlan;
using renderer::vulkan::staging_publish_plan;

int main() {
    struct Case {
        bool coherent;
        bool deduplicate;
        StagingPublishPlan expected;
    };
    // A keeps the established publication sequence. B must retain Vulkan
    // publication for non-coherent memory and the coherent platform workaround.
    constexpr std::array cases{
        Case{ false, false, { true, true, false } },
        Case{ false, true, { false, true, true } },
        Case{ true, false, { true, false, false } },
        Case{ true, true, { true, false, false } },
    };
    for (const auto &test : cases) {
        const auto plan = staging_publish_plan(test.coherent, test.deduplicate);
        assert(plan.platform_flush == test.expected.platform_flush);
        assert(plan.vulkan_flush == test.expected.vulkan_flush);
        assert(plan.deduplicated == test.expected.deduplicated);
        assert(plan.platform_flush || plan.vulkan_flush);
        assert(test.coherent || plan.vulkan_flush);
        assert(!test.coherent || plan.platform_flush);
    }

    // Replay an ABBA upload sequence: all non-coherent uploads must publish
    // through Vulkan, while exactly B's duplicate platform operations disappear.
    constexpr std::array phases{ false, true, true, false };
    for (const bool coherent : { false, true }) {
        uint32_t platform_flushes = 0;
        uint32_t vulkan_flushes = 0;
        uint32_t deduplicated_uploads = 0;
        for (const bool phase_b : phases) {
            const auto snapshot = staging_publish_plan(coherent, phase_b);
            platform_flushes += snapshot.platform_flush;
            vulkan_flushes += snapshot.vulkan_flush;
            deduplicated_uploads += snapshot.deduplicated;
        }
        assert(platform_flushes == (coherent ? 4 : 2));
        assert(vulkan_flushes == (coherent ? 0 : 4));
        assert(deduplicated_uploads == (coherent ? 0 : 2));
    }
}
