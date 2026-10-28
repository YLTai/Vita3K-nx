// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.

#include <util/switch_memory_capabilities.h>
#include <util/switch_memory_validation.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(__SWITCH__) && defined(VITA3K_SWITCH_TRACELAB)
#include <malloc.h>
#include <switch.h>

namespace switch_validation {
namespace {

constexpr size_t PageSize = 0x1000;
constexpr uint32_t PermissionIterations = 32;
constexpr const char *Suite = "capability";

struct Scratch {
    uint8_t *allocation = nullptr;
    uint8_t *data = nullptr;
    size_t size = 0;
};

// Failed cleanups must not hand still-borrowed/protected storage back to malloc.
// Keep ownership for the remainder of this process; the shared report records
// the failure and addresses. The suite is called once, and retains at most six
// allocations and one JIT object.
struct RetainedAllocation {
    Scratch scratch;
    void *alias = nullptr;
    VirtmemReservation *reservation = nullptr;
};
RetainedAllocation retained_allocations[8];
size_t retained_count = 0;
Jit retained_jit{};
bool have_retained_jit = false;
bool allow_chainload = true;

void retain(Scratch &scratch, void *alias = nullptr, VirtmemReservation *reservation = nullptr) noexcept {
    allow_chainload = false;
    if (retained_count < sizeof(retained_allocations) / sizeof(retained_allocations[0]))
        retained_allocations[retained_count++] = { scratch, alias, reservation };
    // Even if the diagnostic owner table is full, deliberately do not free it.
    scratch = {};
}

void report_result(const char *test, Result rc, uint64_t elapsed = 0, const char *context = "", const char *failure_status = "skip") {
    char detail[384];
    std::snprintf(detail, sizeof(detail), "rc=0x%08X %s", static_cast<unsigned>(rc), context);
    record(Suite, test, R_SUCCEEDED(rc) ? "pass" : failure_status, detail, elapsed, 1);
}

bool query(const char *test, const void *ptr, MemoryInfo &info) {
    uint32_t page_info = 0;
    const Result rc = svcQueryMemory(&info, &page_info, reinterpret_cast<uint64_t>(ptr));
    char detail[384];
    if (R_FAILED(rc)) {
        std::snprintf(detail, sizeof(detail), "address=%p rc=0x%08X", ptr, static_cast<unsigned>(rc));
        record(Suite, test, "skip", detail);
        return false;
    }
    // MemoryInfo.type is the low eight-bit type, NOT the internal memory-state
    // capability flags. In particular, bit 8/15 cannot report CanReprotect/Alias.
    std::snprintf(detail, sizeof(detail),
        "address=%p base=0x%llX bytes=0x%llX type=0x%X perm=0x%X attr=0x%X ipc_refs=%u device_refs=%u page_info=0x%X; capability_bits=not_exported",
        ptr, static_cast<unsigned long long>(info.addr), static_cast<unsigned long long>(info.size),
        static_cast<unsigned>(info.type & MemState_Type), static_cast<unsigned>(info.perm),
        static_cast<unsigned>(info.attr), static_cast<unsigned>(info.ipc_refcount),
        static_cast<unsigned>(info.device_refcount), static_cast<unsigned>(page_info));
    record(Suite, test, "pass", detail);
    return true;
}

// Verify the ENTIRE range; a successful query of its first page is insufficient
// after split mappings or a partially failed cleanup. This helper never reads
// the queried memory and is also safe for a borrowed source.
bool range_has_permission(const void *ptr, size_t size, uint32_t permission, bool ordinary_heap = false) noexcept {
    const uint64_t first = reinterpret_cast<uint64_t>(ptr);
    const uint64_t end = first + size;
    if (!ptr || end <= first)
        return false;
    for (uint64_t cursor = first; cursor < end;) {
        MemoryInfo info{};
        uint32_t page_info = 0;
        if (R_FAILED(svcQueryMemory(&info, &page_info, cursor)) || info.perm != permission)
            return false;
        if (ordinary_heap && ((info.type & MemState_Type) != MemType_Heap || info.attr != 0))
            return false;
        const uint64_t next = info.addr + info.size;
        if (cursor < info.addr || next <= cursor)
            return false;
        cursor = next < end ? next : end;
    }
    return true;
}

uint8_t expected_byte(size_t offset) {
    return static_cast<uint8_t>((offset * 37u + (offset >> 12) * 11u) ^ 0xA7u);
}

bool allocate_scratch(Scratch &scratch, size_t pages, const char *test) {
    scratch.size = pages * PageSize;
    // Protect only interior pages. Both malloc boundary metadata and our guard
    // pages remain RW even while the test payload is read-only.
    scratch.allocation = static_cast<uint8_t *>(memalign(PageSize, scratch.size + 2 * PageSize));
    if (!scratch.allocation) {
        record(Suite, test, "skip", "isolated scratch allocation failed");
        scratch = {};
        return false;
    }
    scratch.data = scratch.allocation + PageSize;
    std::memset(scratch.allocation, 0x6D, PageSize);
    std::memset(scratch.data + scratch.size, 0xB2, PageSize);
    for (size_t i = 0; i < scratch.size; ++i)
        scratch.data[i] = expected_byte(i);
    if (!range_has_permission(scratch.data, scratch.size, Perm_Rw, true)) {
        record(Suite, test, "skip", "allocation is not an ordinary RW Heap range with clear attributes; no permissions changed");
        std::free(scratch.allocation);
        scratch = {};
        return false;
    }
    return true;
}

bool intact(const Scratch &scratch, bool alias_marker = false) {
    for (size_t i = 0; i < PageSize; ++i) {
        if (scratch.allocation[i] != 0x6D || scratch.data[scratch.size + i] != 0xB2)
            return false;
    }
    for (size_t i = 0; i < scratch.size; ++i) {
        const uint8_t expected = alias_marker && i == 0 ? 0xE3 : expected_byte(i);
        if (scratch.data[i] != expected)
            return false;
    }
    return true;
}

bool cleanup_scratch(Scratch &scratch, Result *restore_result = nullptr) noexcept {
    if (!scratch.allocation)
        return true;
    // Do not write/free until the complete payload is confirmed accessible. A
    // failed restoration is reported and retried once at this cleanup boundary.
    if (!range_has_permission(scratch.data, scratch.size, Perm_Rw, true)) {
        const Result rc = svcSetMemoryPermission(scratch.data, scratch.size, Perm_Rw);
        if (restore_result)
            *restore_result = rc;
        if (R_FAILED(rc) || !range_has_permission(scratch.data, scratch.size, Perm_Rw, true)) {
            retain(scratch);
            return false;
        }
    }
    std::free(scratch.allocation);
    scratch = {};
    return true;
}

bool release_scratch(Scratch &scratch, const char *test) {
    const Scratch original = scratch;
    Result restore_rc = 0;
    if (!cleanup_scratch(scratch, &restore_rc)) {
        char detail[256];
        std::snprintf(detail, sizeof(detail), "restore rc=0x%08X allocation=%p payload=%p bytes=%zu; retained, not freed; chainload disabled",
            static_cast<unsigned>(restore_rc), original.allocation, original.data, original.size);
        record(Suite, test, "fail", detail);
        return false;
    }
    record(Suite, test, "pass", "payload confirmed RW and allocation released");
    return true;
}

// These guards only invoke nonthrowing C APIs and retain resources on failure.
// They also cover unexpected early exits while a reporting/allocation operation
// unwinds. The alias guard is constructed after the scratch guard, so it must
// first return the borrowed source before the scratch destructor may free it.
struct ScratchCleanup {
    Scratch &scratch;
    ~ScratchCleanup() noexcept { cleanup_scratch(scratch); }
};

struct AliasCleanup {
    Scratch &scratch;
    void *&alias;
    VirtmemReservation *&reservation;
    bool mapped = false;
    bool active = true;

    ~AliasCleanup() noexcept {
        if (!active)
            return;
        if (mapped && R_FAILED(svcUnmapMemory(alias, scratch.data, scratch.size))) {
            retain(scratch, alias, reservation);
            return;
        }
        if (reservation) {
            virtmemLock();
            virtmemRemoveReservation(reservation);
            virtmemUnlock();
            reservation = nullptr;
        }
    }
};

struct JitCleanup {
    Jit &jit;
    bool active = false;
    ~JitCleanup() noexcept {
        if (active && R_FAILED(jitClose(&jit))) {
            retained_jit = jit;
            have_retained_jit = true;
            allow_chainload = false;
        }
    }
};

void query_info(const char *test, InfoType type) {
    uint64_t value = 0;
    const Result rc = svcGetInfo(&value, type, CUR_PROCESS_HANDLE, 0);
    char detail[192];
    std::snprintf(detail, sizeof(detail), "rc=0x%08X value=0x%llX", static_cast<unsigned>(rc), static_cast<unsigned long long>(value));
    record(Suite, test, R_SUCCEEDED(rc) ? "pass" : "skip", detail);
}

void environment() {
    const uint32_t version = hosversionGet();
    char detail[384];
    std::snprintf(detail, sizeof(detail), "HOS=%u.%u.%u Atmosphere=%s applet_type=%u module=%s heap_override=%s tick_hz=%llu compiler=%s",
        HOSVER_MAJOR(version), HOSVER_MINOR(version), HOSVER_MICRO(version),
        hosversionIsAtmosphere() ? "yes" : "no-or-unknown", static_cast<unsigned>(appletGetAppletType()),
        envIsNso() ? "NSO" : "NRO", envHasHeapOverride() ? "yes" : "no",
        static_cast<unsigned long long>(armGetSystemTickFreq()), __VERSION__);
    record(Suite, "environment", "pass", detail);
#ifdef VITA3K_LIBNX_BUILD_VERSION
    record(Suite, "libnx_build_version", "pass", VITA3K_LIBNX_BUILD_VERSION);
#else
    record(Suite, "libnx_build_version", "skip", "not embedded; define VITA3K_LIBNX_BUILD_VERSION from the build's libnx package/version");
#endif
    char loader[161]{};
    const char *loader_info = envGetLoaderInfo();
    const uint64_t loader_size = envGetLoaderInfoSize();
    if (loader_info) {
        const size_t count = loader_size < sizeof(loader) - 1 ? static_cast<size_t>(loader_size) : sizeof(loader) - 1;
        for (size_t i = 0; i < count; ++i) {
            const unsigned char c = static_cast<unsigned char>(loader_info[i]);
            loader[i] = c >= 0x20 && c < 0x7F ? static_cast<char>(c) : ' ';
        }
    }
    record(Suite, "loader_info", loader_info && loader_size ? "pass" : "skip", loader[0] ? loader : "loader did not provide identification text");
    std::snprintf(detail, sizeof(detail), "SetMemoryPermission=%u MapMemory=%u UnmapMemory=%u ReturnFromException=%u; loader hints are not capability proofs",
        envIsSyscallHinted(0x02) ? 1u : 0u, envIsSyscallHinted(0x04) ? 1u : 0u,
        envIsSyscallHinted(0x05) ? 1u : 0u, envIsSyscallHinted(0x28) ? 1u : 0u);
    record(Suite, "syscall_hints", "pass", detail);
    query_info("heap_region_address", InfoType_HeapRegionAddress);
    query_info("heap_region_size", InfoType_HeapRegionSize);
    query_info("stack_region_address", InfoType_StackRegionAddress);
    query_info("stack_region_size", InfoType_StackRegionSize);
    query_info("alias_region_address", InfoType_AliasRegionAddress);
    query_info("system_resource_bytes", InfoType_SystemResourceSizeTotal);
    query_info("user_exception_context", InfoType_UserExceptionContextAddress);
    query_info("debugger_attached", InfoType_DebuggerAttached);
    record(Suite, "query_memory_semantics", "pass", "MemoryInfo.type exports the low 8-bit type only; CanReprotect/CanAlias are not inferred from absent internal state bits");
}

void heap_permissions(size_t pages) {
    char test[80];
    std::snprintf(test, sizeof(test), "heap_%zu_pages", pages);
    Scratch scratch;
    ScratchCleanup cleanup{ scratch };
    if (!allocate_scratch(scratch, pages, test))
        return;
    MemoryInfo info{};
    query(test, scratch.data, info);
    uint64_t elapsed = 0;
    uint32_t completed = 0;
    Result last_ro = 0;
    Result last_rw = 0;
    bool permissions_ok = true;
    for (uint32_t i = 0; i < PermissionIterations; ++i) {
        const uint64_t start = armGetSystemTick();
        last_ro = svcSetMemoryPermission(scratch.data, scratch.size, Perm_R);
        elapsed += armGetSystemTick() - start;
        if (R_FAILED(last_ro)) {
            permissions_ok = false;
            break;
        }
        // Query only; there is deliberately no store while the range is RO.
        const bool read_only = range_has_permission(scratch.data, scratch.size, Perm_R, true);
        const uint64_t restore_start = armGetSystemTick();
        last_rw = svcSetMemoryPermission(scratch.data, scratch.size, Perm_Rw);
        elapsed += armGetSystemTick() - restore_start;
        if (R_FAILED(last_rw) || !read_only || !range_has_permission(scratch.data, scratch.size, Perm_Rw, true)) {
            permissions_ok = false;
            break;
        }
        ++completed;
    }
    char detail[384];
    std::snprintf(detail, sizeof(detail), "pages=%zu bytes=%zu completed_roundtrips=%u requested=%u RO_rc=0x%08X RW_rc=0x%08X; elapsed counts SVC calls only, excludes queries",
        pages, scratch.size, completed, PermissionIterations, static_cast<unsigned>(last_ro), static_cast<unsigned>(last_rw));
    const char *status = permissions_ok && completed == PermissionIterations ? "pass" : (R_FAILED(last_ro) ? "skip" : "fail");
    record(Suite, test, status, detail, elapsed, completed);
    std::snprintf(test, sizeof(test), "heap_%zu_pages_data", pages);
    if (range_has_permission(scratch.data, scratch.size, Perm_Rw, true))
        record(Suite, test, intact(scratch) ? "pass" : "fail", "full payload pattern and both unprotected guard pages checked after RW restoration");
    else
        record(Suite, test, "skip", "payload not confirmed RW after the test; no payload access attempted");
    std::snprintf(test, sizeof(test), "heap_%zu_pages_cleanup", pages);
    release_scratch(scratch, test);
}

void stack_alias() {
    Scratch scratch;
    ScratchCleanup cleanup{ scratch };
    if (!allocate_scratch(scratch, 1, "alias_scratch"))
        return;
    void *alias = nullptr;
    VirtmemReservation *reservation = nullptr;
    AliasCleanup alias_cleanup{ scratch, alias, reservation };
    Result map_rc = 0;
    uint64_t map_ticks = 0;
    virtmemLock();
    alias = virtmemFindStack(scratch.size, PageSize);
    if (alias)
        reservation = virtmemAddReservation(alias, scratch.size);
    if (reservation) {
        const uint64_t start = armGetSystemTick();
        map_rc = svcMapMemory(alias, scratch.data, scratch.size);
        alias_cleanup.mapped = R_SUCCEEDED(map_rc);
        map_ticks = armGetSystemTick() - start;
        if (R_FAILED(map_rc)) {
            virtmemRemoveReservation(reservation);
            reservation = nullptr;
        }
    }
    virtmemUnlock();
    if (!reservation) {
        if (R_FAILED(map_rc))
            report_result("stack_alias_map", map_rc, map_ticks);
        else
            record(Suite, "stack_alias_map", "skip", "no isolated Stack destination/reservation available");
        release_scratch(scratch, "stack_alias_cleanup");
        return;
    }
    report_result("stack_alias_map", map_rc, map_ticks);
    // QueryMemory examines metadata. Never dereference or free the borrowed
    // source until UnmapMemory succeeds and it is confirmed RW again.
    MemoryInfo source_info{};
    if (query("alias_source_borrowed", scratch.data, source_info))
        record(Suite, "alias_source_inaccessible", source_info.perm == Perm_None ? "pass" : "fail", "source permissions inspected without reading source bytes");
    MemoryInfo alias_info{};
    query("alias_destination", alias, alias_info);
    const uint64_t protect_start = armGetSystemTick();
    const Result ro_rc = svcSetMemoryPermission(alias, scratch.size, Perm_R);
    report_result("stack_alias_set_RO", ro_rc, armGetSystemTick() - protect_start,
        "denial is a capability result for this mapping type; no protected write is attempted");
    if (R_SUCCEEDED(ro_rc)) {
        record(Suite, "stack_alias_RO_query", range_has_permission(alias, scratch.size, Perm_R) ? "pass" : "fail", "entire destination range checked using QueryMemory");
        const uint64_t restore_start = armGetSystemTick();
        const Result rw_rc = svcSetMemoryPermission(alias, scratch.size, Perm_Rw);
        report_result("stack_alias_restore_RW", rw_rc, armGetSystemTick() - restore_start, "", "fail");
    }
    bool wrote_marker = false;
    if (range_has_permission(alias, scratch.size, Perm_Rw)) {
        static_cast<volatile uint8_t *>(alias)[0] = 0xE3;
        wrote_marker = true;
        record(Suite, "stack_alias_RW_store", static_cast<volatile uint8_t *>(alias)[0] == 0xE3 ? "pass" : "fail", "only the verified RW destination was accessed");
    } else {
        record(Suite, "stack_alias_RW_store", "skip", "destination is not confirmed RW; no store attempted");
    }
    const uint64_t unmap_start = armGetSystemTick();
    const Result unmap_rc = svcUnmapMemory(alias, scratch.data, scratch.size);
    if (R_SUCCEEDED(unmap_rc))
        alias_cleanup.mapped = false;
    report_result("stack_alias_unmap", unmap_rc, armGetSystemTick() - unmap_start, "", "fail");
    if (R_FAILED(unmap_rc)) {
        char detail[256];
        std::snprintf(detail, sizeof(detail), "allocation=%p source=%p alias=%p bytes=%zu; live alias/reservation/backing retained, no source access/free",
            scratch.allocation, scratch.data, alias, scratch.size);
        retain(scratch, alias, reservation);
        alias_cleanup.active = false;
        record(Suite, "stack_alias_cleanup", "fail", detail);
        return;
    }
    virtmemLock();
    virtmemRemoveReservation(reservation);
    virtmemUnlock();
    reservation = nullptr;
    query("alias_source_restored", scratch.data, source_info);
    if (range_has_permission(scratch.data, scratch.size, Perm_Rw, true))
        record(Suite, "stack_alias_data_restored", intact(scratch, wrote_marker) ? "pass" : "fail", "full payload and guards verified after successful unmap; alias marker included if written");
    else
        record(Suite, "stack_alias_data_restored", "skip", "source not confirmed ordinary RW Heap; no payload access attempted");
    release_scratch(scratch, "stack_alias_cleanup");
}

void explicit_write(volatile uint32_t *target, uint32_t value, uint32_t &callbacks) {
    ++callbacks;
    *target = value;
}

void native_jit_store() {
#if defined(__aarch64__)
    // Use libnx's CodeMemory path only. The older process-code fallback has a
    // different failure/rollback contract and is unnecessary for this analogy.
    if (!envIsSyscallHinted(0x4B) || !envIsSyscallHinted(0x4C)) {
        record(Suite, "native_jit_store", "skip", "CodeMemory JIT not hinted; process-code fallback not exercised by the safe capability suite");
        return;
    }
    Jit jit{};
    JitCleanup cleanup{ jit };
    const Result create_rc = jitCreate(&jit, PageSize);
    if (R_FAILED(create_rc)) {
        char detail[192];
        std::snprintf(detail, sizeof(detail), "jitCreate rc=0x%08X; native analogy unavailable in this loader environment", static_cast<unsigned>(create_rc));
        record(Suite, "native_jit_store", "skip", detail);
        return;
    }
    cleanup.active = true;
    const Result writable_rc = jitTransitionToWritable(&jit);
    if (R_SUCCEEDED(writable_rc) && range_has_permission(jitGetRwAddr(&jit), PageSize, Perm_Rw)) {
        // AArch64: str w1, [x0]; ret. This demonstrates an explicit callback is
        // not automatically called by native stores, not Dynarmic correctness.
        constexpr uint32_t instructions[] = { 0xB9000001u, 0xD65F03C0u };
        std::memcpy(jitGetRwAddr(&jit), instructions, sizeof(instructions));
        const Result executable_rc = jitTransitionToExecutable(&jit);
        if (R_SUCCEEDED(executable_rc) && range_has_permission(jitGetRxAddr(&jit), PageSize, Perm_Rx)) {
            using Writer = void (*)(volatile uint32_t *, uint32_t);
            void *const code = jitGetRxAddr(&jit);
            Writer writer = nullptr;
            static_assert(sizeof(writer) == sizeof(code));
            std::memcpy(&writer, &code, sizeof(writer));
            volatile uint32_t value = 0;
            uint32_t callbacks = 0;
            explicit_write(&value, 0x11112222u, callbacks);
            writer(&value, 0x55AA3311u);
            record(Suite, "native_jit_store", value == 0x55AA3311u && callbacks == 1 ? "pass" : "fail",
                "RW target: native STR changed data while explicit callback count stayed one; this is not a Dynarmic path test");
        } else {
            report_result("native_jit_executable", executable_rc, 0, "execution skipped unless entire code page is confirmed RX");
            if (R_SUCCEEDED(executable_rc))
                record(Suite, "native_jit_RX_query", "fail", "executable transition succeeded but code page was not confirmed RX; no code executed");
            record(Suite, "native_jit_store", "skip", "executable transition or RX validation failed");
        }
    } else {
        report_result("native_jit_writable", writable_rc, 0, "code generation skipped unless code page is confirmed RW");
        if (R_SUCCEEDED(writable_rc))
            record(Suite, "native_jit_RW_query", "fail", "writable transition succeeded but code page was not confirmed RW; no code written");
        record(Suite, "native_jit_store", "skip", "writable transition or RW validation failed");
    }
    const Result close_rc = jitClose(&jit);
    cleanup.active = false;
    if (R_FAILED(close_rc)) {
        retained_jit = jit;
        have_retained_jit = true;
        allow_chainload = false;
    }
    report_result("native_jit_cleanup", close_rc, 0, R_FAILED(close_rc) ? "JIT mapping/backing retained; chainload disabled" : "JIT mapping and backing released", "fail");
#else
    record(Suite, "native_jit_store", "skip", "native instruction probe requires an AArch64 host");
#endif
}

} // namespace

void run_memory_capability_validation() {
    // Protect against accidental repeated integration calls, especially after
    // an allocation/mapping was intentionally retained following failed cleanup.
    static bool ran = false;
    if (ran) {
        record(Suite, "run_once", "skip", "memory capability suite already ran in this process");
        return;
    }
    ran = true;
    environment();
    constexpr size_t page_counts[] = { 1, 4, 16, 64 };
    for (size_t pages : page_counts)
        heap_permissions(pages);
    stack_alias();
    native_jit_store();
    record(Suite, "resumable_write_fault", "skip", "not tested: default libnx handler is not a resumable store handler; requires separately validated custom exception entry and complete context restoration");
    record(Suite, "direct_heap_guest_backing", "skip", "scratch Heap checks only; guest backing, page table, aliases and renderer mappings remain unchanged");
    record(Suite, "GPU_IPC_protected_writes", "skip", "CPU permission results do not establish device/IPC write tracking or GPU-import reprotection support");
    char detail[192];
    std::snprintf(detail, sizeof(detail), "retained_allocations=%zu retained_JIT=%u allow_chainload=%u; permission/fault capabilities must be evaluated independently",
        retained_count, have_retained_jit ? 1u : 0u, allow_chainload ? 1u : 0u);
    record(Suite, "cleanup_summary", retained_count == 0 && !have_retained_jit ? "pass" : "fail", detail);
}

bool memory_capabilities_allow_chainload() {
    return allow_chainload;
}

} // namespace switch_validation

#else

namespace switch_validation {
void run_memory_capability_validation() {
    record("capability", "platform", "skip", "Horizon memory capabilities require a Switch TraceLab build");
}
bool memory_capabilities_allow_chainload() {
    return true;
}
} // namespace switch_validation

#endif
