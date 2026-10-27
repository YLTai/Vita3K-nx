// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
// Licensed under GPLv2 or any later version.

#include <renderer/vulkan/descriptor_binding_cache.h>

#include <array>
#include <cstdlib>
#include <iostream>
#include <random>

using renderer::vulkan::DescriptorBindingCache;

static void check(bool condition) {
    if (!condition) {
        std::cerr << "Descriptor binding replay failed\n";
        std::exit(1);
    }
}

struct Snapshot {
    uint64_t layout = 1;
    std::array<uint64_t, 4> sets{ 10, 20, 30, 40 };
    std::array<uint32_t, 4> offsets{ 256, 512, 768, 1024 };
    uint32_t offset_count = 2;
};

// Replay the emitted range as a command buffer would consume it, then compare
// every descriptor/active dynamic offset with the old full-bind path. Layout
// changes deliberately discard all old bindings, which is stricter than the
// compatible-prefix reuse that Vulkan permits.
struct Replay {
    DescriptorBindingCache<uint64_t, uint64_t> cache;
    Snapshot bound{ 0, {}, {}, 0 };

    void reset() {
        cache.invalidate();
        bound = { 0, {}, {}, 0 };
    }

    renderer::vulkan::DescriptorBindRange draw(const Snapshot &desired) {
        const auto range = cache.update(desired.layout, desired.sets,
            desired.offsets, desired.offset_count);
        check(range.first_set + range.set_count <= desired.sets.size());
        if (bound.layout != desired.layout) {
            check(range.first_set == 0 && range.set_count == 4);
            bound = { desired.layout, {}, {}, 0 };
        }
        for (uint32_t i = range.first_set; i < range.first_set + range.set_count; ++i)
            bound.sets[i] = desired.sets[i];
        if (range.set_count && range.first_set == 0) {
            bound.offset_count = desired.offset_count;
            for (uint32_t i = 0; i < desired.offset_count; ++i)
                bound.offsets[i] = desired.offsets[i];
        }
        check(bound.sets == desired.sets);
        check(bound.offset_count == desired.offset_count);
        for (uint32_t i = 0; i < desired.offset_count; ++i)
            check(bound.offsets[i] == desired.offsets[i]);
        return range;
    }
};

int main() {
    // Exhaust all changed-set and changed-offset combinations for both mapping
    // modes, including offsets that are inactive in the two-offset mode.
    for (uint32_t count : { 2U, 4U }) {
        for (uint32_t set_mask = 0; set_mask < 16; ++set_mask) {
            for (uint32_t offset_mask = 0; offset_mask < 16; ++offset_mask) {
                Replay replay;
                Snapshot input;
                input.offset_count = count;
                replay.draw(input);
                for (uint32_t i = 0; i < 4; ++i) {
                    if (set_mask & (1U << i))
                        input.sets[i] += 100;
                    if (offset_mask & (1U << i))
                        input.offsets[i] += 256;
                }
                const auto range = replay.draw(input);
                const bool changed = set_mask || (offset_mask & ((1U << count) - 1));
                check((range.set_count != 0) == changed);
                check(replay.draw(input).set_count == 0);
            }
        }
    }

    Replay replay;
    Snapshot input;
    replay.draw(input);
    input.offsets[0] += 256;
    auto range = replay.draw(input);
    check(range.first_set == 0 && range.set_count == 1);
    input.sets[3] += 1;
    range = replay.draw(input);
    check(range.first_set == 3 && range.set_count == 1);
    input.sets[1] += 1;
    input.sets[3] += 1;
    range = replay.draw(input);
    check(range.first_set == 1 && range.set_count == 3);

    // Same handles after a command-buffer reset, layout A/B/A, frame descriptor
    // reuse, empty texture sets, and changing the active dynamic-offset count.
    std::mt19937 random(0x564b2026);
    for (uint32_t iteration = 0; iteration < 10000; ++iteration) {
        switch (random() % 7) {
        case 0: replay.reset(); break;
        case 1: input.layout = 1 + random() % 4; break;
        case 2: input.offset_count = input.offset_count == 2 ? 4 : 2; break;
        case 3: input.sets[random() % 4] = 1 + random() % 8; break;
        case 4: input.offsets[random() % 4] = (random() % 16) * 256; break;
        case 5: input.sets[2] = input.sets[3] = 1; break;
        default: break;
        }
        replay.draw(input);
    }
    std::cout << "Descriptor binding replay checks passed\n";
}
