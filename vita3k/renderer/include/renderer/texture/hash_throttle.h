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
};

constexpr uint8_t HASH_THROTTLE_WARMUP_SCENES = 8;

// The interval is in scene epochs (not calls or host frames). A guest write
// can arrive at any scene boundary, so this decision is intentionally UNSAFE
// and must remain opt-in with forced full re-hashing for periodic audits.
constexpr bool should_throttle_hash(const HashThrottleState &state,
    uint64_t scene, uint32_t audit_interval) {
    return (audit_interval >= 2 && scene != 0 && !state.ever_changed
        && state.stable_verifications >= HASH_THROTTLE_WARMUP_SCENES
        && state.last_verified_scene != 0
        && scene > state.last_verified_scene
        && (scene - state.last_verified_scene) < audit_interval);
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
    if (scene == 0 || (state.last_verified_scene != 0
        && scene < state.last_verified_scene)) {
        // Scene timestamps can restart on a different GXM context. A
        // previously warmed state must not be trusted across that boundary.
        state.stable_verifications = 0;
    }
    if (changed) {
        // A dynamically updated texture is never throttled again in this
        // cache slot, even if the experiment remains globally enabled.
        state.ever_changed = true;
        state.stable_verifications = 0;
    } else if (scene != 0 && scene != state.last_verified_scene
        && state.last_verified_scene != 0
        && state.stable_verifications < std::numeric_limits<uint8_t>::max()) {
        ++state.stable_verifications;
    }
    state.last_verified_scene = scene;
    state.skipped_since_verification = false;
    return audit_mismatch;
}

} // namespace renderer::texture
