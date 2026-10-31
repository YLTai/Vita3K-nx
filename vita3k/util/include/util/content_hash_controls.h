// Isolated controls run with the actual renderer's Hash function on device.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <util/content_hash_memo.h>
#include <array>

namespace switch_validation::content {
inline constexpr std::array<const char *, 12> control_names{
    "cold_and_equal", "first_middle_last_byte", "palette_only", "same_address_reuse",
    "key_includes_spans", "bounded_lru", "oversize_and_small_fallback", "audit_every_hit",
    "source_changes_during_hash", "private_snapshot_hash_binding", "collision_still_refreshes", "allocation_independent_reset"
};
inline constexpr uint32_t controls_mask = (1u << control_names.size()) - 1;
namespace controls_detail {
inline thread_local Hash underlying = nullptr;
inline thread_local uint8_t *mutate = nullptr;
inline uint64_t mutate_source(const void *p, size_t n) {
    if (mutate) { *mutate ^= 0x5a; mutate = nullptr; }
    return underlying(p, n);
}
inline uint64_t collision(const void *, size_t) { return 7; }
}

template<class Report>
void run_controls(Hash hash, Report report) {
    std::array<uint8_t, 512> pixels{}, palette{}, other{};
    for (size_t i = 0; i < pixels.size(); ++i) { pixels[i] = uint8_t(i * 19 + 3); palette[i] = uint8_t(i * 7); other[i] = uint8_t(i * 11); }
    const Input in{pixels.data(), pixels.size(), palette.data(), 64};
    const auto reference = [&] { return full_digest(in, hash); };
    Memo memo(2048, 8, 1); Counts c;
    bool ok = memo.evaluate(in, Mode::Compare, hash, c) == reference();
    ok &= memo.evaluate(in, Mode::Compare, hash, c) == reference() && c.hits == 1 && c.refreshes == 1;
    report(control_names[0], ok);
    ok = true;
    for (size_t off : {size_t(0), pixels.size() / 2, pixels.size() - 1}) {
        pixels[off] ^= 0x55;
        ok &= memo.evaluate(in, Mode::Compare, hash, c) == reference();
    }
    report(control_names[1], ok && c.refreshes == 4);
    palette[63] ^= 1;
    report(control_names[2], memo.evaluate(in, Mode::Compare, hash, c) == reference() && c.refreshes == 5);
    pixels.fill(0xac); palette.fill(0x37);
    report(control_names[3], memo.evaluate(in, Mode::Compare, hash, c) == reference());
    const Input different{pixels.data(), 128, palette.data(), 128};
    report(control_names[4], memo.evaluate(different, Mode::Compare, hash, c) == full_digest(different, hash));
    {
        Memo bounded(1024, 1, 1); Counts b;
        const Input a{pixels.data(), 512}, d{other.data(), 512};
        bounded.evaluate(a, Mode::Compare, hash, b); bounded.evaluate(d, Mode::Compare, hash, b);
        const auto result = bounded.evaluate(a, Mode::Compare, hash, b);
        report(control_names[5], result == full_digest(a, hash) && b.evictions == 2 && bounded.peak_bytes() <= 1024);
    }
    {
        Memo bounded(32, 8, 16); Counts b;
        report(control_names[6], bounded.evaluate(in, Mode::Compare, hash, b) == reference()
            && bounded.evaluate({pixels.data(), 4}, Mode::Compare, hash, b) == hash(pixels.data(), 4)
            && b.fallback_calls == 2 && bounded.resident_bytes() == 0);
    }
    {
        Memo audit(2048, 8, 1); Counts b;
        for (int i = 0; i < 8; ++i) ok &= audit.evaluate(in, Mode::Audit, hash, b) == reference();
        report(control_names[7], ok && b.audit_checks == 8 && b.hits == 7 && !b.audit_disagreements);
    }
    {
        // Mutation happens after the copy and while the snapshot is hashed.
        // The subsequent independent source Hash must disagree and disarm memo.
        Memo audit(2048, 8, 1); Counts b;
        controls_detail::underlying = hash; controls_detail::mutate = pixels.data();
        const auto result = audit.evaluate(in, Mode::Audit, controls_detail::mutate_source, b);
        report(control_names[8], result == reference() && b.audit_disagreements == 1 && audit.disabled());
    }
    {
        Memo snapshot(2048, 8, 1); Counts b;
        const auto before = reference();
        controls_detail::underlying = hash; controls_detail::mutate = pixels.data();
        const auto result = snapshot.evaluate(in, Mode::Compare, controls_detail::mutate_source, b);
        const auto next = snapshot.evaluate(in, Mode::Compare, hash, b);
        report(control_names[9], result == before && next == reference() && b.refreshes == 2 && b.hits == 0);
    }
    {
        Memo collision(2048, 8, 1); Counts b;
        const Input no_palette{pixels.data(), 512};
        const auto one = collision.evaluate(no_palette, Mode::Compare, controls_detail::collision, b);
        pixels[1] ^= 1;
        const auto two = collision.evaluate(no_palette, Mode::Compare, controls_detail::collision, b);
        report(control_names[10], one == 7 && two == 7 && b.refreshes == 2 && b.hits == 0);
        collision.reset();
        report(control_names[11], !collision.disabled() && collision.resident_bytes() == 0 && collision.peak_bytes() == 0);
    }
    controls_detail::underlying = nullptr; controls_detail::mutate = nullptr;
}
} // namespace switch_validation::content
