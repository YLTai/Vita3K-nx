#!/usr/bin/env python3
"""Exercise the real capability suite's ownership and failure paths on Linux.

The small libnx declaration shim supplies metadata and injects failures. Heap
read-only protection and inaccessible borrowed sources use real mprotect, so an
accidental payload access terminates the child. Linker wrappers also reject
freeing a protected/borrowed allocation or freeing a scratch allocation twice.

This checks control flow and cleanup, not Horizon permission/alias semantics:
the host alias is a separate mmap copied back on unmap. No AArch64 instructions
execute, and the CodeMemory capability is explicitly unavailable in the shim.
Each scenario gets a fresh process because the production suite runs once.
"""
import argparse
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

SWITCH_HEADER = r'''
#pragma once
#include <cstddef>
#include <cstdint>
using u64 = std::uint64_t;
using Result = std::uint32_t;
using Handle = std::uint32_t;
constexpr Handle CUR_PROCESS_HANDLE = 1;
#define R_SUCCEEDED(x) ((x) == 0)
#define R_FAILED(x) ((x) != 0)
#define HOSVER_MAJOR(x) (((x) >> 16) & 255u)
#define HOSVER_MINOR(x) (((x) >> 8) & 255u)
#define HOSVER_MICRO(x) ((x) & 255u)
enum {
    MemState_Type = 255, MemType_Heap = 5,
    Perm_None = 0, Perm_R = 1, Perm_Rw = 3, Perm_Rx = 5
};
enum InfoType {
    InfoType_HeapRegionAddress, InfoType_HeapRegionSize,
    InfoType_StackRegionAddress, InfoType_StackRegionSize,
    InfoType_AliasRegionAddress, InfoType_SystemResourceSizeTotal,
    InfoType_UserExceptionContextAddress, InfoType_DebuggerAttached
};
struct MemoryInfo {
    u64 addr, size;
    std::uint32_t type, attr, perm, ipc_refcount, device_refcount, padding;
};
struct VirtmemReservation { unsigned value; };
struct Jit { void *rw_addr; void *rx_addr; };
Result svcQueryMemory(MemoryInfo *, std::uint32_t *, std::uint64_t);
Result svcSetMemoryPermission(void *, std::size_t, std::uint32_t);
Result svcGetInfo(std::uint64_t *, InfoType, Handle, std::uint64_t);
Result svcMapMemory(void *, void *, std::size_t);
Result svcUnmapMemory(void *, void *, std::size_t);
std::uint64_t armGetSystemTick();
std::uint64_t armGetSystemTickFreq();
std::uint32_t hosversionGet();
bool hosversionIsAtmosphere();
int appletGetAppletType();
bool envIsNso();
bool envHasHeapOverride();
const char *envGetLoaderInfo();
std::uint64_t envGetLoaderInfoSize();
bool envIsSyscallHinted(unsigned);
void virtmemLock();
void virtmemUnlock();
void *virtmemFindStack(std::size_t, std::size_t);
VirtmemReservation *virtmemAddReservation(void *, std::size_t);
void virtmemRemoveReservation(VirtmemReservation *);
Result jitCreate(Jit *, std::size_t);
Result jitTransitionToWritable(Jit *);
Result jitTransitionToExecutable(Jit *);
Result jitClose(Jit *);
inline void *jitGetRwAddr(Jit *jit) { return jit->rw_addr; }
inline void *jitGetRxAddr(Jit *jit) { return jit->rx_addr; }
'''

HARNESS = r'''
#include <switch.h>
#include <util/switch_memory_capabilities.h>
#include <util/switch_memory_validation.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <stdexcept>
#include <string>
#include <sys/mman.h>

namespace {
constexpr std::size_t PageSize = 4096;
constexpr Result Denied = 0xD401;
struct Allocation {
    void *base = nullptr;
    std::uintptr_t payload = 0;
    std::size_t payload_size = 0;
    bool freed = false;
};
struct RecordedTest {
    char name[96]{};
    char status[8]{};
    char detail[512]{};
};
Allocation allocations[8];
std::size_t allocation_count = 0;
RecordedTest records[128];
std::size_t record_count = 0;
std::map<std::uintptr_t, std::uint32_t> permissions;
std::string scenario;
std::uintptr_t borrowed = 0, destination = 0, denied_restore = 0;
std::size_t alias_size = 0;
unsigned lock_depth = 0;
bool reservation_live = false;
VirtmemReservation reservation{};

void check(bool condition, const char *message) {
    if (!condition) {
        std::fprintf(stderr, "capability scenario %s: %s\n", scenario.c_str(), message);
        std::abort();
    }
}

std::uint32_t permission_at(std::uintptr_t address) {
    const auto it = permissions.find(address & ~std::uintptr_t(PageSize - 1));
    return it == permissions.end() ? static_cast<std::uint32_t>(Perm_Rw) : it->second;
}

Allocation *owner(std::uintptr_t address, std::size_t size) {
    for (std::size_t i = 0; i < allocation_count; ++i) {
        auto &allocation = allocations[i];
        if (!allocation.freed && address >= allocation.payload
            && address + size <= allocation.payload + allocation.payload_size)
            return &allocation;
    }
    return nullptr;
}

void protect(void *address, std::size_t size, std::uint32_t permission) {
    int native_permission = PROT_NONE;
    if (permission & Perm_R) native_permission |= PROT_READ;
    if (permission & 2u) native_permission |= PROT_WRITE;
    check(mprotect(address, size, native_permission) == 0, "host mprotect failed");
    const auto first = reinterpret_cast<std::uintptr_t>(address);
    for (std::size_t offset = 0; offset < size; offset += PageSize)
        permissions[first + offset] = permission;
}

const RecordedTest *find_test(const char *name) {
    // Some diagnostics intentionally share a name; the final status wins.
    for (std::size_t i = record_count; i; --i)
        if (std::strcmp(records[i - 1].name, name) == 0)
            return &records[i - 1];
    return nullptr;
}

void expect_status(const char *name, const char *status) {
    const auto *test = find_test(name);
    if (!test || std::strcmp(test->status, status) != 0) {
        std::fprintf(stderr, "expected %s=%s, got %s\n", name, status,
            test ? test->status : "missing");
        std::abort();
    }
}
} // namespace

// Wrap only references in these executable translation units. Scratch storage
// still comes from the system allocator; the wrappers enforce ownership before
// handing it back, independently of whether glibc happens to touch its payload.
extern "C" void *__real_memalign(std::size_t, std::size_t);
extern "C" void __real_free(void *);
extern "C" void *__wrap_memalign(std::size_t alignment, std::size_t size) {
    check(alignment == PageSize && size >= 3 * PageSize, "unexpected scratch layout");
    check(allocation_count < 8, "scratch allocation accounting overflow");
    void *base = __real_memalign(alignment, size);
    check(base != nullptr, "host scratch allocation failed");
    auto &allocation = allocations[allocation_count++];
    allocation = { base, reinterpret_cast<std::uintptr_t>(base) + PageSize,
        size - 2 * PageSize, false };
    return base;
}
extern "C" void __wrap_free(void *pointer) {
    for (std::size_t i = allocation_count; i; --i) {
        auto &allocation = allocations[i - 1];
        if (allocation.base != pointer) continue;
        check(!allocation.freed, "scratch allocation freed twice");
        for (std::size_t offset = 0; offset < allocation.payload_size; offset += PageSize)
            check(permission_at(allocation.payload + offset) == Perm_Rw,
                "protected/borrowed scratch storage passed to free");
        check(allocation.payload != borrowed, "borrowed source passed to free");
        allocation.freed = true;
        break;
    }
    __real_free(pointer);
}

Result svcQueryMemory(MemoryInfo *info, std::uint32_t *page_info, std::uint64_t address) {
    // This particular query runs after a successful map and outside the suite's
    // noexcept permission helper. It exercises C++ scope unwinding with a live
    // alias; real svcQueryMemory does not throw C++ exceptions.
    if (borrowed && address == borrowed && scenario.find("throw_query") == 0)
        throw std::runtime_error("injected unwind after alias map");
    const auto page = static_cast<std::uintptr_t>(address) & ~std::uintptr_t(PageSize - 1);
    *info = {};
    info->addr = page;
    info->size = PageSize;
    info->type = page == destination ? 11 : MemType_Heap;
    info->attr = page == borrowed ? 1 : 0;
    info->perm = permission_at(page);
    *page_info = 0;
    return 0;
}

Result svcSetMemoryPermission(void *pointer, std::size_t size, std::uint32_t permission) {
    const auto address = reinterpret_cast<std::uintptr_t>(pointer);
    check(address % PageSize == 0 && size && size % PageSize == 0, "unaligned permission request");
    if (address == destination) return Denied;
    check(owner(address, size) != nullptr, "permission request escaped scratch payload");
    if (scenario == "deny_RO" && permission == Perm_R) return Denied;
    if (scenario == "deny_restore" && !denied_restore && permission == Perm_R)
        denied_restore = address;
    if (address == denied_restore && permission == Perm_Rw) return Denied;
    protect(pointer, size, permission);
    return 0;
}

Result svcMapMemory(void *alias, void *source, std::size_t size) {
    check(lock_depth == 1 && reservation_live && !borrowed, "invalid alias-map ownership");
    check(reinterpret_cast<std::uintptr_t>(alias) == destination && size == alias_size,
        "alias map escaped reservation");
    check(owner(reinterpret_cast<std::uintptr_t>(source), size) != nullptr,
        "alias source escaped isolated allocation");
    std::memcpy(alias, source, size);
    borrowed = reinterpret_cast<std::uintptr_t>(source);
    protect(source, size, Perm_None);
    return 0;
}

Result svcUnmapMemory(void *alias, void *source, std::size_t size) {
    check(reservation_live && reinterpret_cast<std::uintptr_t>(source) == borrowed,
        "unmap has no matching borrowed source");
    check(reinterpret_cast<std::uintptr_t>(alias) == destination && size == alias_size,
        "unmap has no matching alias");
    if (scenario == "deny_unmap" || scenario == "throw_query_deny_unmap") return Denied;
    protect(source, size, Perm_Rw);
    std::memcpy(source, alias, size);
    borrowed = 0;
    return 0;
}

Result svcGetInfo(std::uint64_t *value, InfoType, Handle, std::uint64_t) { *value = 0; return 0; }
std::uint64_t armGetSystemTick() { static std::uint64_t tick = 0; return ++tick; }
std::uint64_t armGetSystemTickFreq() { return 19200000; }
std::uint32_t hosversionGet() { return (23u << 16) | 1u; }
bool hosversionIsAtmosphere() { return true; }
int appletGetAppletType() { return 0; }
bool envIsNso() { return false; }
bool envHasHeapOverride() { return false; }
const char *envGetLoaderInfo() { return "Linux ownership test"; }
std::uint64_t envGetLoaderInfoSize() { return std::strlen(envGetLoaderInfo()); }
bool envIsSyscallHinted(unsigned number) { return number != 0x4B && number != 0x4C; }

void virtmemLock() { check(lock_depth == 0, "recursive virtual-memory lock"); ++lock_depth; }
void virtmemUnlock() { check(lock_depth == 1, "unbalanced virtual-memory unlock"); --lock_depth; }
void *virtmemFindStack(std::size_t size, std::size_t alignment) {
    check(lock_depth == 1 && !destination && alignment == PageSize, "invalid Stack reservation request");
    void *alias = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    check(alias != MAP_FAILED, "host mmap failed");
    destination = reinterpret_cast<std::uintptr_t>(alias);
    alias_size = size;
    return alias;
}
VirtmemReservation *virtmemAddReservation(void *alias, std::size_t size) {
    check(lock_depth == 1 && !reservation_live
        && reinterpret_cast<std::uintptr_t>(alias) == destination && size == alias_size,
        "invalid reservation ownership");
    reservation_live = true;
    return &reservation;
}
void virtmemRemoveReservation(VirtmemReservation *entry) {
    check(lock_depth == 1 && entry == &reservation && reservation_live,
        "reservation removed twice or outside lock");
    check(!borrowed, "live alias reservation removed before source restoration");
    check(munmap(reinterpret_cast<void *>(destination), alias_size) == 0, "host munmap failed");
    reservation_live = false;
    destination = 0;
    alias_size = 0;
}

Result jitCreate(Jit *, std::size_t) { std::abort(); }
Result jitTransitionToWritable(Jit *) { std::abort(); }
Result jitTransitionToExecutable(Jit *) { std::abort(); }
Result jitClose(Jit *) { std::abort(); }

namespace switch_validation {
void record(const char *suite, const char *test, const char *status, const char *detail,
    std::uint64_t, std::uint64_t) noexcept {
    check(std::strcmp(suite, "capability") == 0, "wrong suite name");
    check(lock_depth == 0, "reporting while holding virtual-memory lock");
    check(record_count < 128, "record accounting overflow");
    check(std::strcmp(status, "pass") == 0 || std::strcmp(status, "fail") == 0
        || std::strcmp(status, "skip") == 0, "invalid result status");
    auto &entry = records[record_count++];
    std::snprintf(entry.name, sizeof(entry.name), "%s", test);
    std::snprintf(entry.status, sizeof(entry.status), "%s", status);
    std::snprintf(entry.detail, sizeof(entry.detail), "%s", detail);
}
} // namespace switch_validation

int main(int argc, char **argv) {
    check(argc == 2, "one scenario argument required");
    scenario = argv[1];
    const bool expected_unwind = scenario.find("throw_query") == 0;
    const bool expected_retained = scenario == "deny_restore" || scenario == "deny_unmap"
        || scenario == "throw_query_deny_unmap";
    const bool expected_alias_retained = scenario == "deny_unmap" || scenario == "throw_query_deny_unmap";
    check(scenario == "baseline" || scenario == "deny_RO" || scenario == "deny_restore"
        || scenario == "deny_unmap" || scenario == "throw_query" || scenario == "throw_query_deny_unmap",
        "unknown scenario");
    check(switch_validation::memory_capabilities_allow_chainload(), "chainload is not initially allowed");
    bool caught_unwind = false;
    try {
        switch_validation::run_memory_capability_validation();
    } catch (const std::runtime_error &error) {
        check(expected_unwind && std::strcmp(error.what(), "injected unwind after alias map") == 0,
            "unexpected exception");
        caught_unwind = true;
    }
    check(caught_unwind == expected_unwind, "expected cleanup unwind was not exercised");
    check(switch_validation::memory_capabilities_allow_chainload() == !expected_retained,
        "retained storage did not disable chainload, or clean storage disabled it");
    check(lock_depth == 0, "virtual-memory lock leaked");
    check(allocation_count == 5, "suite did not run all four Heap sizes and Stack alias");
    std::size_t retained_count = 0;
    for (std::size_t i = 0; i < allocation_count; ++i)
        retained_count += allocations[i].freed ? 0 : 1;
    check(retained_count == (expected_retained ? 1u : 0u), "unexpected retained/freed scratch count");
    check(bool(borrowed) == expected_alias_retained && reservation_live == expected_alias_retained
        && bool(destination) == expected_alias_retained, "alias/backing/reservation lifetime mismatch");
    if (expected_alias_retained)
        check(permission_at(borrowed) == Perm_None, "failed unmap did not leave source protected");
    if (scenario == "deny_restore")
        check(permission_at(denied_restore) == Perm_R, "failed restoration did not leave payload read-only");

    expect_status("environment", "pass");
    expect_status("query_memory_semantics", "pass");
    constexpr std::size_t page_counts[] = { 1, 4, 16, 64 };
    for (const auto pages : page_counts) {
        const bool failed_restore = scenario == "deny_restore" && pages == 1;
        char name[80];
        std::snprintf(name, sizeof(name), "heap_%zu_pages", pages);
        expect_status(name, scenario == "deny_RO" ? "skip" : failed_restore ? "fail" : "pass");
        std::snprintf(name, sizeof(name), "heap_%zu_pages_data", pages);
        expect_status(name, failed_restore ? "skip" : "pass");
        std::snprintf(name, sizeof(name), "heap_%zu_pages_cleanup", pages);
        expect_status(name, failed_restore ? "fail" : "pass");
    }
    expect_status("stack_alias_map", "pass");
    if (!expected_unwind) {
        expect_status("alias_source_inaccessible", "pass");
        expect_status("stack_alias_set_RO", "skip");
        expect_status("stack_alias_unmap", expected_alias_retained ? "fail" : "pass");
        expect_status("stack_alias_cleanup", expected_alias_retained ? "fail" : "pass");
        if (!expected_alias_retained) expect_status("stack_alias_data_restored", "pass");
        expect_status("native_jit_store", "skip");
        expect_status("resumable_write_fault", "skip");
        expect_status("cleanup_summary", expected_retained ? "fail" : "pass");
    }
    if (!expected_retained) {
        for (std::size_t i = 0; i < record_count; ++i)
            check(std::strcmp(records[i].status, "fail") != 0, "unexpected failure result");
    }
    // Retry must not reprobe retained storage or allocate another fixture.
    const auto before_retry = record_count;
    switch_validation::run_memory_capability_validation();
    expect_status("run_once", "skip");
    check(record_count == before_retry + 1 && allocation_count == 5, "suite reran after once guard");
    check(switch_validation::memory_capabilities_allow_chainload() == !expected_retained,
        "retry cleared the retention gate");
    std::printf("Capability scenario %s OK (retained=%zu, unwind=%s)\n",
        scenario.c_str(), retained_count, caught_unwind ? "yes" : "no");
}
'''


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cxx", default="c++")
    args = parser.parse_args()
    if sys.platform != "linux" or os.sysconf("SC_PAGE_SIZE") != 4096:
        print("SKIP capability ownership gate: requires Linux with 4096-byte pages")
        return
    with tempfile.TemporaryDirectory(prefix="vita3k-capabilities-") as directory:
        tmp = Path(directory)
        (tmp / "switch.h").write_text(SWITCH_HEADER)
        (tmp / "test.cpp").write_text(HARNESS)
        executable = tmp / "capability-test"
        subprocess.run([
            args.cxx, "-std=c++20", "-O2", "-Wall", "-Wextra", "-Werror", "-pedantic",
            "-D__SWITCH__", "-D__aarch64__", "-DVITA3K_SWITCH_TRACELAB=1",
            "-I", str(tmp), "-I", str(ROOT / "vita3k/util/include"),
            str(ROOT / "vita3k/util/src/switch_memory_capabilities.cpp"),
            str(tmp / "test.cpp"), "-Wl,--wrap=memalign", "-Wl,--wrap=free",
            "-o", str(executable),
        ], cwd=tmp, check=True)
        for scenario in (
            "baseline", "deny_RO", "deny_restore", "deny_unmap",
            "throw_query", "throw_query_deny_unmap",
        ):
            subprocess.run([str(executable), scenario], cwd=tmp, check=True)
    print("Capability ownership gate OK (six real-mprotect scenarios; not a Horizon capability result)")


if __name__ == "__main__":
    main()
