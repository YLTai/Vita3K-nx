// Deterministic positive/negative challenges against the production observer.
// Device adapter uses the real Dynarmic JIT; host adapter deliberately does not.
// Scratch bytes only. No Guest game data, Vulkan or filesystem is touched.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <util/switch_write_watch.h>
#include <cstdio>
#include <utility>

namespace switch_validation::watch {
template<class Store, class Report>
void run_challenges(Table &table, Page *raw, Page first, Page second,
    uint32_t guest, uint32_t alias, Store store_guest, Report report) {
    // Two separate 32-bit words are packed into one 64-bit signature. This
    // comparison is exact for these fixtures; it is NOT a texture XXH3 test.
    const auto word = [](Page p) { uint32_t v; std::memcpy(&v, p, 4); return v; };
    const auto set = [](Page p, uint32_t v) { std::memcpy(p, &v, 4); };
    Page pixel = first + 128, palette = second + 128;
    const auto signature = [&] { return uint64_t(word(pixel)) | uint64_t(word(palette)) << 32; };
    const auto hash = [&] {
        auto t = table.before(raw, guest + 128, pixel, 4, palette, 4);
        return std::pair{t, table.after(t, signature())};
    };
    const auto prepare = [&] {
        table.phase(no_index, false);
        set(pixel, 0x12345678); set(palette, 0x76543210);
        table.phase(0, false, true); hash();
        table.phase(1, true); hash();
    };
    const auto result = [&](const char *name, bool ok, const char *detail) {
        report(name, ok && !table.snapshot().failed, detail);
    };
    const auto counts = [&] { return table.snapshot().phases[1]; };

    prepare();
    bool ran = store_guest(guest + 128, 0x98765432, 4); hash();
    result("native_in_range", ran && word(pixel) == 0x98765432 && counts().changed_cpu_seen == 1
        && counts().first_write_pages == 1 && counts().cpu_range_hits == 1,
        "changed payload; first native store must be observed");

    prepare();
    ran = store_guest(guest + 128, word(pixel), 4); hash();
    result("native_same_value", ran && counts().first_write_pages == 1 && counts().changed == 0,
        "a write event need not change bytes; no false content-change claim");

    prepare();
    ran = store_guest(guest + 64, 0x11111111, 4); hash();
    const auto before = counts();
    // First outside-footprint write opens the page. A later direct payload
    // store is unobserved, so all dependencies must remain conservatively tainted.
    ran &= store_guest(guest + 128, 0x22222222, 4); hash();
    result("page_false_share_then_payload", ran && before.cpu_page_only_hits == 1
        && before.cpu_range_hits == 0 && before.changed == 0
        && counts().first_write_pages == 1 && counts().clean_candidate_checks == 1
        && counts().changed_cpu_seen == 1,
        "exact-range evidence must not authorize clean reuse after whole-page reopening");

    prepare();
    ran = store_guest(alias + 128, 0x33333333, 4); hash();
    result("native_alias", ran && word(pixel) == 0x33333333 && counts().changed_cpu_seen == 1
        && counts().first_write_pages == 1, "software alias resolves to the same backing");

    prepare();
    ran = store_guest(guest + page_size + 128, 0x44444444, 4); hash();
    result("native_palette_only", ran && word(pixel) == 0x12345678
        && word(palette) == 0x44444444 && counts().changed_cpu_seen == 1,
        "separate palette page must participate in the dependency");

    prepare();
    set(palette, 0x55555555); hash();
    result("negative_raw_host", counts().changed_unobserved == 1 && counts().changed_cpu_seen == 0,
        "EXPECTED one unknown change: raw host writers must not be silently certified");

    for (uint32_t hook = 0; hook < static_cast<uint32_t>(HleHook::Count); ++hook) {
        prepare();
        set(pixel, 0x66666666);
        table.hle_completed(raw, pixel, 4, static_cast<HleHook>(hook)); hash();
        char name[64]; std::snprintf(name, sizeof(name), "selected_hle_notice_%u", hook);
        result(name, counts().changed_hle_only == 1 && counts().changed_unobserved == 0
            && counts().hle_hooks[hook] == 1 && counts().first_write_pages == 0,
            "completion API only; not a claim that every HLE export is covered");
    }

    prepare();
    set(first + 64, 0x77777777);
    table.hle_completed(raw, first + 64, 4, HleHook::LibcMemset);
    set(pixel, 0x88888888); hash(); // Deliberately omitted notice for the payload.
    result("negative_hle_page_neighbor", counts().hle_page_only_hits == 1
        && counts().hle_range_hits == 0 && counts().changed_unobserved == 1,
        "an unrelated HLE write on the page cannot explain a payload change");

    prepare();
    Page *writes = table.bind(raw);
    table.publish(raw, guest / page_size, raw[guest / page_size]);
    result("idempotent_publish_keeps_watch", writes && !writes[guest / page_size],
        "same-entry PTE publication must not silently open a watched page");

    prepare();
    auto t = table.before(raw, guest + 128, pixel, 4, palette, 4);
    set(pixel, 0x99999999); table.hle_completed(raw, pixel, 4, HleHook::LibcMemcpy);
    table.after(t, signature());
    result("notice_during_hash", counts().hash_raced == 1 && counts().changed == 0,
        "known mutation during Hash invalidates observation, not renderer content Hash");

    prepare();
    t = table.before(raw, guest + 128, pixel, 4, palette, 4);
    const auto original = raw[guest / page_size];
    table.publish(raw, guest / page_size, raw[(guest + page_size) / page_size] + page_size);
    table.after(t, signature());
    const bool remap_discarded = counts().hash_raced == 1 && counts().changed == 0;
    table.publish(raw, guest / page_size, original);
    result("mapping_during_hash", remap_discarded, "old mapping token cannot train a new baseline");

    prepare();
    t = table.before(raw, guest + 128, pixel, 4, palette, 4);
    table.phase(3, false); table.after(t, signature());
    result("phase_boundary_token", table.snapshot().phases[3].stale_tokens == 1,
        "even a B-to-A transition without an epoch change discards the old token");

    prepare();
    bool success = true;
    table.exclusive<uint32_t>(raw, guest + 128, 9, 0, success);
    const bool failed_cas = !success && counts().first_write_pages == 0 && !writes[guest / page_size];
    table.exclusive<uint32_t>(raw, guest + 128, 9, word(pixel), success); hash();
    result("callback_CAS_success_failure", failed_cas && success && counts().changed_cpu_seen == 1,
        "actual callback CAS; not native LDREX/STREX or multi-core reservation coverage");

    prepare();
    ran = store_guest(guest + page_size - 2, 0xabcdef01, 4);
    uint32_t value = 0;
    const auto read = table.read(raw, guest + page_size - 2, &value, 4);
    result("native_cross_page", ran && read == Access::Complete && value == 0xabcdef01
        && counts().first_write_pages == 2,
        "boundary fallback writes both non-contiguous backing pages");

    prepare();
    const auto cached_old = signature();
    set(pixel, 0xaaaaaaaa);
    table.phase(2, true); // Reproduce a new observation baseline after an old cache hash.
    auto receipt = hash();
    table.cache_changed(receipt.first, receipt.second, true, guest + 128, 4, cached_old, signature());
    result("cache_baseline_reconciliation", table.snapshot().phases[2].changed == 0
        && table.snapshot().phases[2].cache_baseline_changes == 1,
        "cache invalidation is not lost when the observer establishes its first baseline");

    prepare();
    const auto prior = signature();
    set(pixel, 0xbbbbbbbb); hash(); // A different use of the same backing sees the new bytes first.
    receipt = hash();
    table.cache_changed(receipt.first, receipt.second, true, guest + 128, 4, prior, signature());
    result("cache_history_reconciliation", counts().cache_shared_changes == 1 && counts().cache_changes == 1,
        "per-cache-entry history and per-backing history are distinct");

    prepare();
    table.cache_changed({}, HashObservation::Ignored, false, guest + 128, 4, 1, 2);
    result("cache_missing_receipt", counts().cache_unmatched_changes == 1,
        "invalid / unsupported hash path remains explicitly unmatched");

    prepare();
    receipt = hash();
    set(pixel, 0xcccccccc); table.hle_completed(raw, pixel, 4, HleHook::ClibMemcpy);
    table.cache_changed(receipt.first, receipt.second, true, guest + 128, 4, 1, 2);
    result("cache_mutated_receipt", counts().cache_raced_changes == 1,
        "a known write between hash completion and cache decision invalidates attribution");
    table.phase(no_index, false);
}
} // namespace switch_validation::watch
