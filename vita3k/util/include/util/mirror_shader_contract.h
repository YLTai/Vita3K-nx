// ABI and bounded ranges shared by the device shader test and host model.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
namespace switch_validation::mirror_shader {
inline constexpr size_t payload = 16384;
inline constexpr size_t guard = 512;
inline constexpr size_t physical = payload + 2 * guard;
inline constexpr uint8_t sentinel = 0xa5;
struct Push {
    uint64_t source = 0, destination = 0;
    uint32_t count = 0, value = 0, operation = 0, reserved = 0;
};
static_assert(sizeof(Push) == 32 && offsetof(Push, count) == 16
    && offsetof(Push, operation) == 24);
struct Range { uint32_t offset, bytes; };
// Includes shifted device addresses, a non-workgroup-sized copy, the final
// word and a zero-length dispatch. No new backing or lifetime is invented.
inline constexpr std::array<Range, 5> copies{{
    {0, payload}, {4, 268}, {4092, 12}, {payload - 4, 4}, {payload, 0}
}};
constexpr bool valid_range(size_t offset, size_t bytes) {
    return !(offset & 3) && !(bytes & 3) && offset <= payload && bytes <= payload - offset;
}
constexpr uint32_t groups(uint32_t words) { return (words + 63) / 64 + 1; }
inline bool guards_intact(std::span<const uint8_t> data, size_t *bad = nullptr) {
    if (data.size() != physical) return false;
    for (size_t i = 0; i < guard; ++i) {
        if (data[i] != sentinel) { if (bad) *bad = i; return false; }
        const auto tail = guard + payload + i;
        if (data[tail] != sentinel) { if (bad) *bad = tail; return false; }
    }
    return true;
}
} // namespace switch_validation::mirror_shader
