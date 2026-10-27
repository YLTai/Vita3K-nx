// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
// Licensed under GPLv2 or any later version.

#include <renderer/vertex_input.h>

#include <cstdlib>
#include <iostream>
#include <map>
#include <random>
#include <set>
#include <vector>

struct Attribute {
    uint32_t regIndex;
    uint32_t streamIndex;
    uint32_t offset;
};

struct AttributeInfo {
    uint32_t component_count;
    bool regformat;
};

static void check(bool condition) {
    if (!condition) {
        std::cerr << "Vertex input metadata check failed\n";
        std::exit(1);
    }
}

// Enumerate the actual used bindings and array elements independently of the
// maximum-offset formula. This also checks that unmatched registers contribute
// neither streams nor offsets.
static void verify(const std::vector<Attribute> &attributes, const std::map<uint32_t, AttributeInfo> &infos) {
    const auto metadata = renderer::make_vertex_input_metadata(attributes, infos);
    std::set<uint32_t> streams;
    std::vector<uint32_t> offsets;
    for (const auto &attribute : attributes) {
        if (!infos.contains(attribute.regIndex))
            continue;
        streams.insert(attribute.streamIndex);
        offsets.push_back(attribute.offset);
        const auto &info = infos.at(attribute.regIndex);
        if (info.regformat) {
            for (uint32_t component = 4; component < info.component_count; component += 4)
                offsets.push_back(attribute.offset + component * uint32_t(sizeof(float)));
        }
    }
    check(metadata.stream_count == (streams.empty() ? 0 : *streams.rbegin() + 1));

    std::vector<uint32_t> limits{ 0, 15, 16, 255, 2047, 65535 };
    for (const auto offset : offsets) {
        limits.push_back(offset);
        if (offset)
            limits.push_back(offset - 1);
        limits.push_back(offset + 1);
    }
    for (const auto limit : limits) {
        const bool requires_attribute_bindings = std::any_of(offsets.begin(), offsets.end(),
            [limit](uint32_t offset) { return offset > limit; });
        check((metadata.max_attribute_offset > limit) == requires_attribute_bindings);
    }
}

int main() {
    verify({}, {});
    verify({ { 7, 15, 65535 } }, {});
    for (const uint32_t components : { 0U, 1U, 4U, 5U, 8U, 9U, 16U }) {
        for (const bool regformat : { false, true }) {
            verify({ { 0, 0, 0 }, { 1, 5, 2047 }, { 2, 15, 65535 } },
                { { 0, { components, regformat } }, { 1, { components, regformat } } });
        }
    }

    std::mt19937 random(0x3c0ffee);
    for (uint32_t trial = 0; trial < 10000; ++trial) {
        std::vector<Attribute> attributes;
        std::map<uint32_t, AttributeInfo> infos;
        for (uint32_t reg = 0; reg < 16; ++reg) {
            if (random() % 2)
                infos.emplace(reg, AttributeInfo{ uint32_t(random() % 33), bool(random() % 2) });
        }
        const uint32_t count = uint32_t(random() % 33);
        for (uint32_t i = 0; i < count; ++i)
            attributes.push_back({ uint32_t(random() % 24), uint32_t(random() % 16), uint32_t(random() % 65536) });
        verify(attributes, infos);
    }
    std::cout << "Vertex input metadata checks passed\n";
}
