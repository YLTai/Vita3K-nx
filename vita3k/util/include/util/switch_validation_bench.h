// Portable plan for the single-launch JIT comparison matrix.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <cstdint>

namespace switch_validation {

enum class StoreBenchMode : uint32_t { Original, SameTableX25, Mirror, FirstWrite, Preopened };
inline constexpr uint32_t store_bench_modes = 5;
inline constexpr uint32_t store_bench_rounds = 5;
inline constexpr uint32_t store_bench_samples = 2 * 2 * store_bench_rounds * store_bench_modes;

constexpr StoreBenchMode store_bench_mode(uint32_t round, uint32_t slot, bool reverse) {
    constexpr std::array<uint32_t, store_bench_modes> base{0, 1, 4, 2, 3};
    const auto position = reverse ? store_bench_modes - 1 - slot : slot;
    return static_cast<StoreBenchMode>((base[position] + round) % store_bench_modes);
}
constexpr uint32_t store_bench_jit(StoreBenchMode mode) {
    return mode == StoreBenchMode::Original ? 0 : mode == StoreBenchMode::SameTableX25 ? 1 : 2;
}
constexpr const char *store_mode_name(StoreBenchMode mode) {
    switch (mode) {
    case StoreBenchMode::Original: return "original_table";
    case StoreBenchMode::SameTableX25: return "same_table_x25";
    case StoreBenchMode::Mirror: return "mirror_write_table";
    case StoreBenchMode::FirstWrite: return "first_write_table";
    case StoreBenchMode::Preopened: return "preopened_write_table";
    }
    return "unknown";
}

} // namespace switch_validation
