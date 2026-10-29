// Isolated scratch-Heap experiment, not a production Guest fault handler.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>

namespace switch_validation {
struct HeapFaultResult {
    uint64_t faults = 0, handler_ticks = 0;
    uintptr_t last_pc = 0, last_far = 0;
    uint32_t dirty_mask = 0, last_rc = 0;
    bool restored = false;
};
struct HeapPermissionResult {
    uint32_t read_rc = 0, restore_rc = 0, before_type = 0, before_attr = 0;
    bool query_before = false, read_only = false, writable_after = false;
};
#if defined(__SWITCH__) && defined(VITA3K_SWITCH_TRACELAB)
bool heap_fault_probe_enabled();
// Synchronous tests only. Each arm/disarm pair belongs to one host thread.
// All ranges must be private page-aligned allocations, not Guest or GPU memory.
bool heap_fault_arm(void *begin, size_t size, uintptr_t expected_pc = 0) noexcept;
HeapFaultResult heap_fault_disarm() noexcept;
// Owner-thread observation only; does not change permissions or consume dirtiness.
HeapFaultResult heap_fault_peek() noexcept;
void run_heap_fault_probe();
bool heap_fault_jit_allowed();
bool heap_fault_checkpoint(const char *test, bool armed) noexcept;
void heap_fault_probe_finished() noexcept;
bool heap_fault_safe_to_chainload() noexcept;
// Capability-only round trip. No CPU/GPU access while the target is read-only.
HeapPermissionResult heap_permission_roundtrip(void *, size_t) noexcept;
void heap_fault_retain_allocation() noexcept;
void copy_heap_fault_checkpoint(const char *directory) noexcept;
#else
inline bool heap_fault_probe_enabled() { return false; }
inline bool heap_fault_arm(void *, size_t, uintptr_t = 0) noexcept { return false; }
inline HeapFaultResult heap_fault_disarm() noexcept { return {}; }
inline HeapFaultResult heap_fault_peek() noexcept { return {}; }
inline void run_heap_fault_probe() {}
inline bool heap_fault_jit_allowed() { return false; }
inline bool heap_fault_checkpoint(const char *, bool) noexcept { return false; }
inline void heap_fault_probe_finished() noexcept {}
inline bool heap_fault_safe_to_chainload() noexcept { return true; }
inline HeapPermissionResult heap_permission_roundtrip(void *, size_t) noexcept { return {}; }
inline void heap_fault_retain_allocation() noexcept {}
inline void copy_heap_fault_checkpoint(const char *) noexcept {}
#endif
} // namespace switch_validation
