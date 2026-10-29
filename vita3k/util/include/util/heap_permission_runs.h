// Normalize a private scratch range without crossing different old permissions.
// Horizon SetMemoryPermission is stricter than POSIX mprotect on mixed R/RW.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <util/heap_fault_policy.h>

namespace switch_validation::fault {
struct PagePermission {
    uint32_t rc = 0;
    bool private_heap = false;
    uint32_t permission = 0;
};
struct PermissionChange {
    uint32_t first_rc = 0, failed_mask = 0;
    uint32_t before_read_mask = 0, before_write_mask = 0, read_mask = 0;
    uint32_t calls = 0, queries = 0, page_retries = 0;
    bool complete = false;
};
// Query/Set use actual SVCs in production. The test adapter also rejects a
// mixed-old-permission range, rather than silently accepting Linux semantics.
// Only a stopped, single-owner scratch allocation may call this routine.
template <typename Query, typename Set>
PermissionChange normalize_private_pages(uintptr_t begin, size_t size,
    uint32_t target, Query query, Set set) noexcept {
    constexpr uint32_t read = 1, rw = 3, bad_state = 0xf601;
    PermissionChange out;
    if (!begin || !size || (begin | size) % page_size || size > max_pages * page_size
        || begin + size < begin || (target != read && target != rw)) {
        out.first_rc = bad_state; out.failed_mask = ~0u; return out;
    }
    const auto pages = static_cast<uint32_t>(size / page_size);
    std::array<uint32_t, max_pages> old{};
    const auto remember = [&](uint32_t rc) { if (rc && !out.first_rc) out.first_rc = rc; };
    for (uint32_t i = 0; i < pages; ++i) {
        ++out.queries;
        const auto p = query(begin + i * page_size);
        if (p.rc || !p.private_heap || (p.permission != read && p.permission != rw)) {
            remember(p.rc ? p.rc : bad_state); continue;
        }
        old[i] = p.permission;
        if (p.permission == read) out.before_read_mask |= 1u << i;
        else out.before_write_mask |= 1u << i;
    }
    for (uint32_t i = 0; i < pages;) {
        if (!old[i] || old[i] == target) { ++i; continue; }
        uint32_t end = i + 1;
        while (end < pages && old[end] == old[i]) ++end;
        ++out.calls;
        const uint32_t rc = set(begin + i * page_size, (end - i) * page_size, target);
        remember(rc);
        // Bounded recovery: a failed multi-page request may have partially
        // succeeded. Retest each page, and never touch an external/pinned page.
        if (rc && end - i > 1) for (uint32_t j = i; j < end; ++j) {
            ++out.queries;
            const auto p = query(begin + j * page_size);
            if (p.rc || !p.private_heap || (p.permission != read && p.permission != rw)) {
                remember(p.rc ? p.rc : bad_state); continue;
            }
            if (p.permission == target) continue;
            ++out.calls; ++out.page_retries;
            remember(set(begin + j * page_size, page_size, target));
        }
        i = end;
    }
    // Return codes alone do not permit free/reuse. Verify every final page.
    for (uint32_t i = 0; i < pages; ++i) {
        ++out.queries;
        const auto p = query(begin + i * page_size);
        if (!p.rc && p.private_heap && p.permission == read) out.read_mask |= 1u << i;
        if (p.rc || !p.private_heap || p.permission != target) {
            out.failed_mask |= 1u << i;
            remember(p.rc ? p.rc : bad_state);
        }
    }
    out.complete = out.failed_mask == 0;
    return out;
}
} // namespace switch_validation::fault
