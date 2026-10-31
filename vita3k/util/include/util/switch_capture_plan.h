// One button, one capture, two writer designs. No Hash skipping.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <cstdint>
namespace switch_validation {
enum class CaptureMode : uint8_t { Open, FirstWrite, Exact };
inline constexpr std::array<CaptureMode, 6> compact_order{
    CaptureMode::Open, CaptureMode::FirstWrite, CaptureMode::Exact,
    CaptureMode::Exact, CaptureMode::FirstWrite, CaptureMode::Open
};
inline constexpr uint32_t compact_settle_seconds = 2;
inline constexpr uint32_t compact_measure_seconds = 10;
inline constexpr uint32_t compact_min_samples = 50;
constexpr const char *capture_mode_name(CaptureMode mode) {
    switch (mode) {
    case CaptureMode::FirstWrite: return "first_write";
    case CaptureMode::Exact: return "exact_range";
    default: return "baseline";
    }
}
constexpr char capture_mode_letter(CaptureMode mode) {
    return mode == CaptureMode::Exact ? 'C' : mode == CaptureMode::FirstWrite ? 'B' : 'A';
}
inline constexpr std::array<const char *, 15> compact_controls{
    "first_write_reopens_aliases",
    "open_page_repeat_negative_control",
    "native_exact_store",
    "native_repeat_alias_store",
    "native_same_value_store",
    "raw_after_cpu_negative_control",
    "page_neighbor_negative_control",
    "host_notice_positive_control",
    "observer_CAS_contract",
    "cache_first_baseline_ledger",
    "cache_shared_history_ledger",
    "remap_discards_inflight_hash",
    "native_palette_only_store",
    "native_crosspage_exact_ranges",
    "canary_cleanup",
};
inline constexpr uint32_t compact_controls_mask = (1u << compact_controls.size()) - 1;
} // namespace switch_validation
