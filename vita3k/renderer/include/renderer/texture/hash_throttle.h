// Conservative state machine for an OPTIONAL, UNSAFE cross-scene texture
// hash-throttling experiment. Missing CPU/GPU dirty tracking means even a
// stable texture can change between audits; never enable in release defaults.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <limits>

namespace renderer::texture {

struct HashThrottleState {
    uint64_t last_verified_scene = 0;
    uint8_t stable_verifications = 0;
    bool ever_changed = false;
    bool skipped_since_verification = false;
    // Deliberately unsafe diagnostic experiment: validation count, not scene
    // distance. A stable texture is sampled once per N cross-scene reuses.
    uint8_t aggressive_confirmed = 0;
    uint8_t aggressive_skips_since_audit = 0;
};

constexpr uint8_t HASH_THROTTLE_WARMUP_SCENES = 8;

// The interval is in scene epochs (not calls or host frames). A guest write
// can arrive at any scene boundary, so this decision is intentionally UNSAFE
// and must remain opt-in with forced full re-hashing for periodic audits.
// Pure decision function. The probe observes the real experiment's gating
// logic without changing the original texture-cache validation policy.
// Exclusive reasons let a single window explain why hot scenes cannot skip.
enum class HashThrottleDecision : uint8_t {
    Eligible = 0,
    Disabled,
    NoEpoch,
    PreviouslyChanged,
    Warmup,
    NoBaseline,
    NonMonotonic,
    AuditDue
};

constexpr HashThrottleDecision classify_hash_throttle(
    const HashThrottleState &state, uint64_t scene, uint32_t audit_interval) {
    if (audit_interval < 2)
        return HashThrottleDecision::Disabled;
    if (scene == 0)
        return HashThrottleDecision::NoEpoch;
    if (state.ever_changed)
        return HashThrottleDecision::PreviouslyChanged;
    if (state.stable_verifications < HASH_THROTTLE_WARMUP_SCENES)
        return HashThrottleDecision::Warmup;
    if (state.last_verified_scene == 0)
        return HashThrottleDecision::NoBaseline;
    if (scene <= state.last_verified_scene)
        return HashThrottleDecision::NonMonotonic;
    if (scene - state.last_verified_scene >= audit_interval)
        return HashThrottleDecision::AuditDue;
    return HashThrottleDecision::Eligible;
}

constexpr bool should_throttle_hash(const HashThrottleState &state,
    uint64_t scene, uint32_t audit_interval) {
    return classify_hash_throttle(state, scene, audit_interval)
        == HashThrottleDecision::Eligible;
}

// Aggressive A/B ignores the distance between two scene epochs. In the
// measured heavy scene, that distance is ~9 for each source frame, so the
// old 8-epoch gate never skipped a single hash. This strategy deliberately
// tolerates missed Guest/HLE/GPU writes until the next periodic audit.
constexpr bool should_skip_by_reuse_count(const HashThrottleState &state,
    uint64_t scene, uint32_t verify_every) {
    return verify_every >= 2 && verify_every <= 64
        && scene != 0 && !state.ever_changed
        && state.last_verified_scene != 0
        && state.aggressive_confirmed >= 1
        && static_cast<uint32_t>(state.aggressive_skips_since_audit) + 1
            < verify_every;
}

inline void note_aggressive_hash_skip(HashThrottleState &state) {
    ++state.aggressive_skips_since_audit;
    state.skipped_since_verification = true;
}

inline void note_throttled_hash(HashThrottleState &state) {
    state.skipped_since_verification = true;
}

// Called only after the entire texture's original hash has actually been
// recomputed. Return true when a change was detected after a prior skip;
// that observation is proof the experiment may have presented stale data.
inline bool note_hash_verification(HashThrottleState &state,
    uint64_t scene, bool changed) {
    const bool audit_mismatch = changed && state.skipped_since_verification;
    const bool reset_epoch = scene == 0
        || (state.last_verified_scene != 0 && scene < state.last_verified_scene);
    if (reset_epoch) {
        // Scene timestamps may rewind when a different GXM context takes over.
        // This full verification is the new baseline, not a stable followup.
        state.stable_verifications = 0;
    }
    if (changed) {
        // A dynamically updated texture is never throttled again in this
        // cache slot, even if the experiment remains globally enabled.
        state.ever_changed = true;
        state.stable_verifications = 0;
        state.aggressive_confirmed = 0;
    } else if (!reset_epoch && scene != state.last_verified_scene
        && state.last_verified_scene != 0
        && state.stable_verifications < std::numeric_limits<uint8_t>::max()) {
        ++state.stable_verifications;
    }
    // Reuse-count eligibility is intentionally independent of the scene
    // epoch gap (and its context-local rewinds). It requires one additional
    // unchanged full verification after the initial cache entry hash.
    if (!changed && state.last_verified_scene != 0 && scene != 0)
        state.aggressive_confirmed = 1;
    state.aggressive_skips_since_audit = 0;
    state.last_verified_scene = scene;
    state.skipped_since_verification = false;
    return audit_mismatch;
}

} // namespace renderer::texture
