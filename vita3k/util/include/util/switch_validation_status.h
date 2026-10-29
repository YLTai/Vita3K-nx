// A copied, race-free view of the memory validation controller.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <cstdint>
#include <cstdio>

namespace switch_validation {

enum class Stage : uint8_t {
    Disabled, Startup, Waiting, Warmup, Settling, Measuring, Paused, Interrupted, Complete
};
enum class CaptureOutcome : uint8_t { Pending, Ready, Inconclusive, Invalid };

// No pointers into the recorder, renderer state, or dynamically allocated text.
// A UI must never inspect the live phase arrays or infer completion from time.
struct StatusSnapshot {
    Stage stage = Stage::Disabled;
    CaptureOutcome outcome = CaptureOutcome::Pending;
    bool automatic = false;
    bool deduplicate = false;
    bool write_shadow = false, watch_failed = false;
    uint64_t watched_pages = 0, first_write_pages = 0, unobserved_changes = 0;
    bool restart_pending = false;
    uint32_t run_id = 0;
    uint32_t phase = 0; // 1-based while active; phase_count after completion.
    uint32_t phase_count = 8;
    uint32_t progress_per_mille = 0;
    uint32_t passed = 0, failed = 0, skipped = 0;
    uint64_t results_dropped = 0, intervals_dropped = 0;
    uint64_t samples = 0, phase_samples = 0;
    uint64_t publish_calls = 0, dedup_calls = 0;
    uint64_t remaining_ms = 0, stage_remaining_ms = 0;
    uint64_t total_ms = 0;
    uint64_t source_tick = 0, timebase_hz = 0;
    bool insufficient_samples = false;
    bool missing_treatment = false;
};

inline const char *outcome_name(CaptureOutcome outcome) {
    switch (outcome) {
    case CaptureOutcome::Ready: return "ready";
    case CaptureOutcome::Inconclusive: return "inconclusive";
    case CaptureOutcome::Invalid: return "invalid";
    default: return "pending";
    }
}

// ASCII labels deliberately do not depend on optional CJK fonts on Horizon.
// Native overlay and host tests use exactly the same presentation rules.
struct HudText {
    enum class Tone { Neutral, Running, Ready, Warning, Error };
    Tone tone = Tone::Neutral;
    std::array<char, 128> title{};
    std::array<char, 128> timing{};
    std::array<char, 128> tests{};
    std::array<char, 128> samples{};
    std::array<char, 128> action{};
};

inline uint64_t ceil_seconds(uint64_t ms) { return ms / 1000 + (ms % 1000 != 0); }

inline HudText make_hud_text(const StatusSnapshot &s, uint64_t now_tick) {
    HudText text;
    const auto phase_left = ceil_seconds(s.stage_remaining_ms);
    const auto left = ceil_seconds(s.remaining_ms);
    std::snprintf(text.tests.data(), text.tests.size(), "Self-tests: %u PASS | %u FAIL | %u SKIP",
        s.passed, s.failed, s.skipped);
    std::snprintf(text.samples.data(), text.samples.size(), "Samples: %llu | B uploads: %llu | Hash: FULL",
        static_cast<unsigned long long>(s.samples), static_cast<unsigned long long>(s.dedup_calls));
    if (s.write_shadow)
        std::snprintf(text.samples.data(), text.samples.size(), "Pages: %llu | First writes: %llu | Unobserved: %llu | FULL Hash",
            static_cast<unsigned long long>(s.watched_pages), static_cast<unsigned long long>(s.first_write_pages),
            static_cast<unsigned long long>(s.unobserved_changes));
    std::snprintf(text.action.data(), text.action.size(), "L+R+Y: start/restart | Keep the scene stable");
    std::snprintf(text.timing.data(), text.timing.size(), "Run %u | %u%% | Remaining %02llu:%02llu",
        s.run_id, s.progress_per_mille / 10,
        static_cast<unsigned long long>(left / 60), static_cast<unsigned long long>(left % 60));
    switch (s.stage) {
    case Stage::Startup:
        std::snprintf(text.title.data(), text.title.size(), "MEMORY VALIDATION | Starting / self-tests");
        break;
    case Stage::Waiting:
        std::snprintf(text.title.data(), text.title.size(), "MEMORY VALIDATION | Ready to start");
        std::snprintf(text.timing.data(), text.timing.size(), "Enter a repeatable heavy scene, then press L+R+Y");
        break;
    case Stage::Warmup:
        std::snprintf(text.title.data(), text.title.size(), "MEMORY VALIDATION | WARM-UP");
        std::snprintf(text.timing.data(), text.timing.size(), "Auto start in %02llu:%02llu | L+R+Y starts now",
            static_cast<unsigned long long>(phase_left / 60), static_cast<unsigned long long>(phase_left % 60));
        break;
    case Stage::Settling:
    case Stage::Measuring:
        text.tone = HudText::Tone::Running;
        std::snprintf(text.title.data(), text.title.size(), "MEMORY VALIDATION | %s | %u/%u %c",
            s.stage == Stage::Settling ? "SETTLING" : "MEASURING", s.phase, s.phase_count,
            s.deduplicate ? 'B' : 'A');
        std::snprintf(text.timing.data(), text.timing.size(), "%u%% | Phase %llus | Total remaining %02llu:%02llu",
            s.progress_per_mille / 10, static_cast<unsigned long long>(phase_left),
            static_cast<unsigned long long>(left / 60), static_cast<unsigned long long>(left % 60));
        break;
    case Stage::Paused:
        text.tone = HudText::Tone::Warning;
        std::snprintf(text.title.data(), text.title.size(), "MEMORY VALIDATION | PAUSED before capture");
        std::snprintf(text.timing.data(), text.timing.size(), "Warm-up restarts after resume");
        break;
    case Stage::Interrupted:
        text.tone = HudText::Tone::Error;
        std::snprintf(text.title.data(), text.title.size(), "MEMORY VALIDATION | INTERRUPTED");
        std::snprintf(text.timing.data(), text.timing.size(), "Current run is invalid; partial results retained");
        std::snprintf(text.action.data(), text.action.size(), "L+R+Y: restart | Or exit normally and share report");
        break;
    case Stage::Complete:
        text.tone = s.outcome == CaptureOutcome::Ready ? HudText::Tone::Ready
            : s.outcome == CaptureOutcome::Invalid ? HudText::Tone::Error : HudText::Tone::Warning;
        std::snprintf(text.title.data(), text.title.size(), "MEMORY VALIDATION | DONE - %s",
            s.outcome == CaptureOutcome::Ready ? "CAPTURE READY"
            : s.outcome == CaptureOutcome::Invalid ? "CHECK REPORT" : "INCONCLUSIVE");
        std::snprintf(text.timing.data(), text.timing.size(), "%s",
            s.outcome == CaptureOutcome::Invalid ? "A test or capture-quality check failed"
            : s.outcome == CaptureOutcome::Inconclusive ? (s.write_shadow ? "No observed texture coverage; share this report" : "No B uploads in at least one phase; no speed claim")
            : "All phases recorded (not proof of graphics correctness)");
        if (s.write_shadow && s.unobserved_changes && s.outcome != CaptureOutcome::Invalid) {
            text.tone = HudText::Tone::Warning;
            std::snprintf(text.timing.data(), text.timing.size(), "Unobserved changes captured; no need to repeat this run");
        }
        std::snprintf(text.action.data(), text.action.size(), "EXIT GAME NORMALLY TO SAVE | Do not force-close");
        break;
    default:
        break;
    }
    // A stalled renderer must not display a timer-driven, fictitious DONE.
    if ((s.stage == Stage::Measuring || s.stage == Stage::Settling || s.stage == Stage::Warmup)
        && s.timebase_hz && s.source_tick && now_tick > s.source_tick
        && (now_tick - s.source_tick) / s.timebase_hz >= 5) {
        text.tone = HudText::Tone::Warning;
        std::snprintf(text.title.data(), text.title.size(), "MEMORY VALIDATION | WAITING FOR SOURCE FRAMES");
        std::snprintf(text.timing.data(), text.timing.size(), "Progress is held at the last completed render loop");
    }
    if (s.restart_pending && s.stage != Stage::Disabled) {
        text.tone = HudText::Tone::Warning;
        std::snprintf(text.title.data(), text.title.size(), "MEMORY VALIDATION | START / RESTART REQUESTED");
        std::snprintf(text.action.data(), text.action.size(), "Waiting for a source frame; do not exit yet");
    }
    return text;
}

} // namespace switch_validation
