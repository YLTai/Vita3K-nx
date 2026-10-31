// Pure admission policy for the isolated Horizon write-fault experiment.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>

namespace switch_validation::fault {
inline constexpr size_t page_size = 4096;
inline constexpr size_t max_pages = 16;
struct Evidence {
    uintptr_t far = 0, pc = 0;
    uint32_t esr = 0, pstate = 0;
};
// Only permission faults on registered scratch memory may resume. Nothing here
// authorizes handling a Guest fault, instruction abort or arbitrary exception.
constexpr bool admissible(const Evidence &e, uintptr_t begin, size_t size,
    uintptr_t expected_pc, uint32_t pending, bool owner_matches) noexcept {
    if (!owner_matches || !begin || !size || (begin | size) % page_size
        || size > max_pages * page_size || begin + size < begin
        || e.far < begin || e.far - begin >= size || (e.pstate & 0x10)
        || (expected_pc && e.pc != expected_pc)) return false;
    const uint32_t ec = e.esr >> 26;
    const uint32_t dfsc = e.esr & 63;
    if (ec != 0x24 || !(e.esr & (1u << 6)) || (e.esr & (1u << 10))
        || dfsc < 0x0d || dfsc > 0x0f) return false;
    return (pending & (1u << ((e.far - begin) / page_size))) != 0;
}
} // namespace switch_validation::fault
