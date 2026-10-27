// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
// Licensed under GPLv2 or any later version.

#pragma once

#include <algorithm>
#include <cstdint>

namespace renderer {

// The patched vertex attributes and shader register information are immutable
// after program creation. Keep their device-independent binding requirements
// with the program instead of walking the register map for every draw.
struct VertexInputMetadata {
    uint32_t stream_count = 0;
    uint32_t max_attribute_offset = 0;
};

template <typename Attributes, typename AttributeInfos>
VertexInputMetadata make_vertex_input_metadata(const Attributes &attributes, const AttributeInfos &infos) {
    VertexInputMetadata metadata;
    for (const auto &attribute : attributes) {
        const auto it = infos.find(attribute.regIndex);
        if (it == infos.end())
            continue;

        metadata.stream_count = std::max(metadata.stream_count, uint32_t(attribute.streamIndex) + 1);
        const auto &info = it->second;
        const uint32_t array_offset = info.regformat && info.component_count > 4
            ? ((info.component_count - 1) / 4) * 16
            : 0;
        metadata.max_attribute_offset = std::max(metadata.max_attribute_offset,
            uint32_t(attribute.offset) + array_offset);
    }
    return metadata;
}

} // namespace renderer
