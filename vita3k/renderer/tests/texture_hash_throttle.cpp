// Offline state-machine tests; no Switch SDK required.
// SPDX-License-Identifier: GPL-2.0-or-later
#include <renderer/texture/hash_throttle.h>
#include <cassert>
#include <cstdint>

using renderer::texture::HashThrottleState;
using renderer::texture::HashThrottleDecision;
using renderer::texture::classify_hash_throttle;
using renderer::texture::HASH_THROTTLE_WARMUP_SCENES;
using renderer::texture::note_hash_verification;
using renderer::texture::note_throttled_hash;
using renderer::texture::should_throttle_hash;
using renderer::texture::should_skip_by_reuse_count;
using renderer::texture::note_aggressive_hash_skip;

int main() {
    HashThrottleState state{};
    assert(!should_throttle_hash(state, 1, 8));
    note_hash_verification(state, 1, false);
    for (uint64_t scene = 2; scene < 2 + HASH_THROTTLE_WARMUP_SCENES; ++scene) {
        assert(!should_throttle_hash(state, scene, 8));
        assert(!note_hash_verification(state, scene, false));
    }
    // First allowed skip occurs only after eight full cross-scene checks.
    const uint64_t first_skippable = state.last_verified_scene + 1;
    assert(should_throttle_hash(state, first_skippable, 8));
    note_throttled_hash(state);
    assert(should_throttle_hash(state, first_skippable + 6, 8));
    assert(!should_throttle_hash(state, first_skippable + 7, 8));
    // The seventh succeeding scene must be audited with a full hash.
    assert(!note_hash_verification(state, first_skippable + 7, false));
    assert(!state.skipped_since_verification);
    assert(should_throttle_hash(state, first_skippable + 8, 8));

    // If an audit observes changed bytes, skipped frames were potentially
    // wrong. The texture is now permanently excluded from throttling.
    note_throttled_hash(state);
    assert(should_throttle_hash(state, first_skippable + 14, 8));
    assert(!should_throttle_hash(state, first_skippable + 15, 8));
    assert(note_hash_verification(state, first_skippable + 15, true));
    assert(state.ever_changed);
    assert(!should_throttle_hash(state, first_skippable + 16, 8));
    for (uint64_t scene = first_skippable + 16; scene < first_skippable + 99; ++scene)
        note_hash_verification(state, scene, false);
    assert(!should_throttle_hash(state, first_skippable + 99, 8));

    // Dynamic textures discovered during warm-up are never throttled.
    HashThrottleState dynamic{};
    note_hash_verification(dynamic, 100, false);
    assert(!note_hash_verification(dynamic, 101, true));
    for (uint64_t scene = 102; scene < 250; ++scene)
        note_hash_verification(dynamic, scene, false);
    assert(!should_throttle_hash(dynamic, 251, 8));

    // Unmapped/reset scene 0, backwards epochs, and disabled interval all
    // force the original full-hash path.
    HashThrottleState reused = state;
    reused = {};
    assert(!should_throttle_hash(reused, 500, 8));
    HashThrottleState stable{};
    note_hash_verification(stable, 500, false);
    for (uint64_t s = 501; s <= 508; ++s)
        note_hash_verification(stable, s, false);
    assert(!should_throttle_hash(stable, 508, 8));
    assert(!should_throttle_hash(stable, 509, 0));
    assert(!should_throttle_hash(stable, 509, 1));
    assert(!should_throttle_hash(stable, 499, 8));
    assert(should_throttle_hash(stable, 509, 8));
    assert(!note_hash_verification(stable, 3, false));
    assert(!should_throttle_hash(stable, 4, 8));
    for (uint64_t scene = 4; scene <= 11; ++scene)
        note_hash_verification(stable, scene, false);
    assert(should_throttle_hash(stable, 12, 8));

    // Each rejected candidate is classified by one exclusive reason.
    using D = HashThrottleDecision;
    HashThrottleState probe{};
    assert(classify_hash_throttle(probe, 100, 0) == D::Disabled);
    assert(classify_hash_throttle(probe, 0, 8) == D::NoEpoch);
    assert(classify_hash_throttle(probe, 100, 8) == D::Warmup);
    probe.stable_verifications = HASH_THROTTLE_WARMUP_SCENES;
    assert(classify_hash_throttle(probe, 100, 8) == D::NoBaseline);
    probe.last_verified_scene = 100;
    assert(classify_hash_throttle(probe, 100, 8) == D::NonMonotonic);
    assert(classify_hash_throttle(probe, 99, 8) == D::NonMonotonic);
    assert(classify_hash_throttle(probe, 108, 8) == D::AuditDue);
    assert(classify_hash_throttle(probe, 132, 8) == D::AuditDue);
    assert(classify_hash_throttle(probe, 101, 8) == D::Eligible);
    probe.ever_changed = true;
    assert(classify_hash_throttle(probe, 101, 8) == D::PreviouslyChanged);

    // Full verification immediately after an epoch rewind creates a fresh
    // baseline; it must not count as one of eight stable forward checks.
    HashThrottleState rewound{};
    rewound.stable_verifications = 8;
    rewound.last_verified_scene = 200;
    note_hash_verification(rewound, 10, false);
    assert(rewound.stable_verifications == 0);
    for (uint64_t epoch = 11; epoch <= 18; ++epoch)
        note_hash_verification(rewound, epoch, false);
    assert(rewound.stable_verifications == 8);
    assert(classify_hash_throttle(rewound, 19, 8) == D::Eligible);

    // AGGRESSIVE A/B: the old scheme never skips when 9 scene epochs elapse
    // between uses; counted reuse should sample the same stable texture
    // once in 8 real lookups regardless of that scene distance.
    HashThrottleState heavy{};
    assert(!should_skip_by_reuse_count(heavy, 1, 8));
    note_hash_verification(heavy, 1, false); // initial cache entry
    assert(!should_skip_by_reuse_count(heavy, 10, 8));
    assert(!note_hash_verification(heavy, 10, false)); // one unchanged full recheck
    assert(!should_throttle_hash(heavy, 19, 8)); // old policy blocks: delta = 9
    assert(should_skip_by_reuse_count(heavy, 19, 8));
    for (uint32_t repeat = 0; repeat < 7; ++repeat) {
        assert(should_skip_by_reuse_count(heavy, 19 + 9 * repeat, 8));
        note_aggressive_hash_skip(heavy);
    }
    assert(heavy.skipped_since_verification);
    assert(heavy.aggressive_skips_since_audit == 7);
    assert(!should_skip_by_reuse_count(heavy, 82, 8)); // eighth use is full audit
    assert(!note_hash_verification(heavy, 82, false));
    assert(!heavy.skipped_since_verification);
    assert(heavy.aggressive_skips_since_audit == 0);
    assert(should_skip_by_reuse_count(heavy, 91, 8));

    note_aggressive_hash_skip(heavy);
    assert(note_hash_verification(heavy, 100, true)); // mismatch after skip
    assert(heavy.ever_changed); // permanently quarantine THIS texture
    assert(!should_skip_by_reuse_count(heavy, 109, 8));
    for (uint64_t epoch = 109; epoch < 190; epoch += 9)
        note_hash_verification(heavy, epoch, false);
    assert(!should_skip_by_reuse_count(heavy, 200, 8));

    // A recycled cache entry must start from the cold baseline.
    heavy = {};
    assert(!should_skip_by_reuse_count(heavy, 200, 8));
    note_hash_verification(heavy, 200, false);
    note_hash_verification(heavy, 209, false);
    assert(should_skip_by_reuse_count(heavy, 218, 8));
    assert(!should_skip_by_reuse_count(heavy, 218, 1));
    assert(!should_skip_by_reuse_count(heavy, 0, 8));
    assert(!should_skip_by_reuse_count(heavy, 218, 65));
    assert(should_skip_by_reuse_count(heavy, 218, 2));
    note_aggressive_hash_skip(heavy);
    assert(!should_skip_by_reuse_count(heavy, 227, 2));
    assert(should_skip_by_reuse_count(heavy, 227, 64));
    return 0;
}
