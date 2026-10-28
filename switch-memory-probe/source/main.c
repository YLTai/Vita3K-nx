// Switch Memory Probe - standalone kernel memory capability diagnostic.
// SPDX-License-Identifier: MIT
// The default path never intentionally faults or writes into protected memory.
#include <switch.h>
#include <malloc.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define PAGE_SIZE 0x1000u
#define MAX_LINES 192
#define TEXT_WIDTH 152
#define SHOW_LINES 22

static char lines[MAX_LINES][TEXT_WIDTH];
static size_t line_count = 0;
static char saved_path[224] = {0};
static uint32_t run_index = 0;
static uint32_t indirect_writes = 0;
static uint32_t global_sample = 0xA1B2C3D4;

static void add(const char *fmt, ...) {
    if (line_count >= MAX_LINES) return;
    va_list args;
    va_start(args, fmt);
    vsnprintf(lines[line_count], sizeof(lines[line_count]), fmt, args);
    va_end(args);
    ++line_count;
}

static const char *mem_type_name(uint32_t type) {
    switch (type & MemState_Type) {
    case MemType_Unmapped: return "Unmapped";
    case MemType_Normal: return "Normal";
    case MemType_CodeStatic: return "CodeStatic";
    case MemType_CodeMutable: return "CodeMutable";
    case MemType_Heap: return "Heap";
    case MemType_SharedMem: return "Shared";
    case MemType_WeirdMappedMem: return "WeirdMapped";
    case MemType_MappedMemory: return "MappedMemory";
    case MemType_ThreadLocal: return "ThreadLocal";
    case MemType_TransferMem: return "TransferMemory";
    case MemType_TransferMemIsolated: return "TransferMemIsolated";
    default: return "Other";
    }
}

static void add_result(const char *label, Result rc) {
    add("%-27s rc=0x%08X %s", label, (unsigned)rc,
        R_SUCCEEDED(rc) ? "OK" : "DENIED/FAILED");
}

static bool query_region(const char *label, const void *ptr, MemoryInfo *out) {
    if (!ptr) {
        add("[%s] NULL (skipped)", label);
        return false;
    }
    MemoryInfo info = {0};
    uint32_t page = 0;
    Result rc = svcQueryMemory(&info, &page, (uint64_t)(uintptr_t)ptr);
    if (R_FAILED(rc)) {
        add("[%s] addr=%p QueryMemory rc=0x%08X", label, ptr, (unsigned)rc);
        return false;
    }
    add("[%s] addr=%p", label, ptr);
    add("  type=%s [state=%08X] perm=%X attr=%X pageinfo=%X",
        mem_type_name(info.type), info.type, info.perm, info.attr, page);
    add("  base=%016llX size=%llX canPerm=%s canMap=%s",
        (unsigned long long)info.addr, (unsigned long long)info.size,
        (info.type & MemState_PermChangeAllowed) ? "yes" : "NO",
        (info.type & MemState_MapAllowed) ? "yes" : "no");
    add("  borrowed=%s locked=%s ref(ipc/dev)=%u/%u",
        (info.attr & MemAttr_IsBorrowed) ? "yes" : "no",
        (info.attr & MemAttr_IsPermissionLocked) ? "yes" : "no",
        info.ipc_refcount, info.device_refcount);
    if (out) *out = info;
    return true;
}

static void query_info(const char *name, InfoType id, Handle handle) {
    u64 value = 0;
    Result rc = svcGetInfo(&value, id, handle, 0);
    if (R_SUCCEEDED(rc))
        add("GetInfo %-27s %016llX", name, (unsigned long long)value);
    else
        add("GetInfo %-27s rc=%08X", name, (unsigned)rc);
}

static void inspect_process(void) {
    uint32_t v = hosversionGet();
    add("=== Process & Kernel ===");
    add("HOS %u.%u.%u  Atmosphere=%s", HOSVER_MAJOR(v), HOSVER_MINOR(v),
        HOSVER_MICRO(v), hosversionIsAtmosphere() ? "yes" : "no/unknown");
    query_info("HeapRegionAddress", InfoType_HeapRegionAddress, CUR_PROCESS_HANDLE);
    query_info("HeapRegionSize", InfoType_HeapRegionSize, CUR_PROCESS_HANDLE);
    query_info("StackRegionAddress", InfoType_StackRegionAddress, CUR_PROCESS_HANDLE);
    query_info("StackRegionSize", InfoType_StackRegionSize, CUR_PROCESS_HANDLE);
    query_info("AliasRegionAddress", InfoType_AliasRegionAddress, CUR_PROCESS_HANDLE);
    query_info("AliasRegionSize", InfoType_AliasRegionSize, CUR_PROCESS_HANDLE);
    query_info("SystemResourceSizeTotal", InfoType_SystemResourceSizeTotal, CUR_PROCESS_HANDLE);
    query_info("UserExceptionContext", InfoType_UserExceptionContextAddress, CUR_PROCESS_HANDLE);
    query_info("DebuggerAttached", InfoType_DebuggerAttached, CUR_PROCESS_HANDLE);
    add("NOTE: exception context presence != resumable fault handler.");
}

static void inspect_existing(void) {
    add("=== Representative mappings ===");
    uint32_t stack_sample = 0xD15EA5ED;
    query_region("global .data", &global_sample, NULL);
    query_region("stack local", &stack_sample, NULL);
    void *p = memalign(PAGE_SIZE, PAGE_SIZE);
    if (!p) { add("heap sample allocation FAILED"); return; }
    memset(p, 0x6b, PAGE_SIZE);
    query_region("heap malloc aligned", p, NULL);
    free(p);
}

static void test_permission_changes(void) {
    add("=== Scratch heap R/W -> R -> R/W ===");
    void *p = memalign(PAGE_SIZE, PAGE_SIZE);
    if (!p) { add("heap permission scratch allocation FAILED"); return; }
    memset(p, 0x73, PAGE_SIZE);
    MemoryInfo orig = {0};
    if (!query_region("heap scratch before", p, &orig) ||
        (orig.perm & Perm_Rw) != Perm_Rw) {
        add("SKIP: scratch is not confirmed readable/writable.");
        free(p);
        return;
    }
    // Only this isolated scratch page is modified; never touch it while RO.
    Result ro = svcSetMemoryPermission(p, PAGE_SIZE, Perm_R);
    add_result("SetMemoryPermission(RO)", ro);
    if (R_SUCCEEDED(ro)) {
        MemoryInfo after = {0};
        bool queried = query_region("scratch after RO", p, &after);
        Result rw = svcSetMemoryPermission(p, PAGE_SIZE, Perm_Rw);
        add_result("SetMemoryPermission(RW)", rw);
        if (R_SUCCEEDED(rw)) {
            query_region("scratch restored", p, NULL);
            add("scratch unchanged: %s", ((volatile uint8_t*)p)[0] == 0x73 ? "yes" : "NO");
            free(p);
        } else {
            add("CRITICAL: restoration failed. Page deliberately leaked, NOT written.");
        }
        if (queried && after.perm != Perm_R)
            add("WARNING: RO call succeeded but QueryMemory perm differs.");
    } else {
        free(p);
    }
}

static void test_stack_alias(void) {
    add("=== Heap source -> Stack-region alias ===");
    void *src = memalign(PAGE_SIZE, PAGE_SIZE);
    if (!src) { add("alias source allocation FAILED"); return; }
    memset(src, 0x6b, PAGE_SIZE);
    query_region("alias source (before)", src, NULL);
    virtmemLock();
    void *dst = virtmemFindStack(PAGE_SIZE, PAGE_SIZE);
    Result mapped = dst ? svcMapMemory(dst, src, PAGE_SIZE) : (Result)0xFFFFFFFF;
    virtmemUnlock();
    add("stack alias dst=%p", dst);
    add_result("svcMapMemory", mapped);
    if (R_FAILED(mapped)) {
        add("Mapping unavailable; no invalid memory was accessed.");
        free(src);
        return;
    }
    // svcMapMemory sets source to inaccessible / borrowed. Never access src.
    query_region("source (borrowed!)", src, NULL);
    query_region("mapped Stack alias", dst, NULL);
    Result ro = svcSetMemoryPermission(dst, PAGE_SIZE, Perm_R);
    add_result("alias SetMemPermission(R)", ro);
    if (R_SUCCEEDED(ro)) {
        query_region("alias after RO", dst, NULL);
        Result restore = svcSetMemoryPermission(dst, PAGE_SIZE, Perm_Rw);
        add_result("alias restore RW", restore);
        if (R_SUCCEEDED(restore))
            query_region("alias restored RW", dst, NULL);
        else
            add("Alias RO state not restored: will NOT attempt write.");
    }
    MemoryInfo now = {0};
    bool can_write = query_region("alias before optional RW", dst, &now)
        && (now.perm & Perm_Rw) == Perm_Rw;
    if (can_write) {
        ((volatile uint8_t*)dst)[0] = 0xE3;
        add("alias write (RW only) = %s", ((volatile uint8_t*)dst)[0] == 0xE3 ? "OK" : "FAILED");
    } else {
        add("alias write SKIPPED due to missing RW permission.");
    }
    Result unmap = svcUnmapMemory(dst, src, PAGE_SIZE);
    add_result("svcUnmapMemory", unmap);
    if (R_SUCCEEDED(unmap)) {
        MemoryInfo restored = {0};
        const bool source_ok = query_region("source restored after Unmap", src, &restored)
            && (restored.perm & Perm_Rw) == Perm_Rw;
        if (source_ok) {
            add("source byte after unmap=%02X (expect E3 if alias was written)",
                ((volatile uint8_t*)src)[0]);
            free(src);
        } else {
            add("Source not confirmed RW after unmap; NOT accessing or freeing.");
        }
    } else {
        add("CRITICAL: unmap failed; source remains borrowed; NOT freeing.");
    }
}

static void tracked_write(volatile uint32_t *p, uint32_t x) {
    ++indirect_writes;
    *p = x;
}

static void test_jit_direct_write(void) {
    add("=== Native AArch64 JIT bypass of explicit callbacks ===");
    Jit j = {0};
    Result rc = jitCreate(&j, PAGE_SIZE);
    add_result("jitCreate", rc);
    if (R_FAILED(rc)) { add("JIT API unavailable; exact Dynarmic path remains untested."); return; }
    add("JIT type=%d rw=%p rx=%p", (int)j.type, jitGetRwAddr(&j), jitGetRxAddr(&j));
    rc = jitTransitionToWritable(&j);
    add_result("jitTransitionToWritable", rc);
    if (R_SUCCEEDED(rc)) {
        // AArch64: str w1, [x0]; ret. No branching beyond returning to caller.
        static const uint32_t code[] = {0xb9000001u, 0xd65f03c0u};
        memcpy(jitGetRwAddr(&j), code, sizeof(code));
        rc = jitTransitionToExecutable(&j);
        add_result("jitTransitionToExecutable", rc);
        if (R_SUCCEEDED(rc)) {
            typedef void (*Writer)(volatile uint32_t *p, uint32_t value);
            union { void *p; Writer func; } entry = {.p = jitGetRxAddr(&j)};
            volatile uint32_t value = 0;
            indirect_writes = 0;
            tracked_write(&value, 0x11112222u);
            uint32_t before = indirect_writes;
            entry.func(&value, 0x55AA3311u);
            add("direct JIT write: %08X callback calls=%u -> %u",
                (unsigned)value, before, (unsigned)indirect_writes);
            add("JIT bypasses standalone callback: %s",
                value == 0x55AA3311u && indirect_writes == before ? "YES" : "UNEXPECTED");
            add("NOTE: this is an analogue; does not run Dynarmic.");
        }
    }
    rc = jitClose(&j);
    add_result("jitClose", rc);
}

static void save_report(const char *phase) {
    if (!saved_path[0]) {
        time_t t = time(NULL);
        snprintf(saved_path, sizeof(saved_path),
            "sdmc:/switch/vita3k-memory-probe-%lld-%u.txt", (long long)t, ++run_index);
    }
    FILE *f = fopen(saved_path, "wb");
    if (!f) {
        add("Could not save report to %s (SD not mounted?)", saved_path);
        return;
    }
    fprintf(f, "Switch Memory Probe v1 / phase=%s\n", phase);
    fprintf(f, "SAFE RUN: queries, heap-permission, alias-permission, native JIT direct store.\n");
    fprintf(f, "Limitations: no guest Dynarmic, no Vulkan/GPU coherence, no verified exception recovery.\n");
    for (size_t i = 0; i < line_count; ++i)
        fprintf(f, "%s\n", lines[i]);
    if (fclose(f) != 0)
        add("WARNING: SD close/write failed");
}

static void run_safe_tests(void) {
    line_count = 0;
    saved_path[0] = 0;
    add("Switch Memory Probe v1 - SAFE CHECKS");
    inspect_process();
    inspect_existing();
    test_permission_changes();
    test_stack_alias();
    test_jit_direct_write();
    add("=== Interpretation ===");
    add("canPerm=NO: svcSetMemoryPermission not advertised for that mapping.");
    add("RO->RW success: permissions change; NOT proof of fault recovery.");
    add("Alias borrowed: guest-like Stack mapping is different from Heap.");
    add("Native JIT direct store bypasses explicit callbacks, not a Dynarmic test.");
    add("GPU/Shader/HLE writes need separate Vita3K integration tests.");
    add("=== End of SAFE test ===");
    save_report("safe");
}

// Opt-in ONLY: protected store normally terminates a retail homebrew.
// This does not prove that a custom kernel-level exception handler is possible.
static void do_explicit_fault(void) {
    add("=== UNSAFE OPT-IN fault injection: read-only scratch write ===");
    void *p = memalign(PAGE_SIZE, PAGE_SIZE);
    if (!p) { add("fault scratch allocation FAILED"); save_report("no-fault"); return; }
    memset(p, 0x41, PAGE_SIZE);
    Result rc = svcSetMemoryPermission(p, PAGE_SIZE, Perm_R);
    add_result("pre-fault SetMemPermission R", rc);
    if (R_FAILED(rc)) {
        add("SKIPPED: permission change not supported, no fault induced.");
        save_report("no-fault");
        free(p);
        return;
    }
    add("ABOUT TO WRITE TO PROTECTED PAGE. If app crashes, no automatic recovery occurred.");
    save_report("pre-fault");
    consoleUpdate(NULL);
    ((volatile uint32_t*)p)[0] = 0xA5A5A5A5u;
    add("PROTECTED WRITE RETURNED: value=%08X", ((volatile uint32_t*)p)[0]);
    Result restore = svcSetMemoryPermission(p, PAGE_SIZE, Perm_Rw);
    add_result("post-fault SetMemPermission RW", restore);
    save_report("post-fault");
    if (R_SUCCEEDED(restore)) free(p);
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    consoleInit(NULL);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);
    run_safe_tests();
    int scroll = 0;
    bool risky_prompt = false;
    while (appletMainLoop()) {
        padUpdate(&pad);
        uint64_t down = padGetButtonsDown(&pad);
        uint64_t held = padGetButtons(&pad);
        if (down & HidNpadButton_Plus) break;
        if (down & HidNpadButton_Down) ++scroll;
        if (down & HidNpadButton_Up) --scroll;
        if (down & HidNpadButton_StickR) scroll = (int)line_count - SHOW_LINES;
        if (down & HidNpadButton_StickL) scroll = 0;
        if (scroll < 0) scroll = 0;
        if (scroll > (int)line_count - SHOW_LINES)
            scroll = line_count > SHOW_LINES ? (int)line_count - SHOW_LINES : 0;
        if (down & HidNpadButton_Y) { run_safe_tests(); scroll = 0; risky_prompt = false; }
        if (down & HidNpadButton_X) risky_prompt = !risky_prompt;
        if (risky_prompt && (down & HidNpadButton_A) &&
            (held & (HidNpadButton_L | HidNpadButton_R | HidNpadButton_ZL | HidNpadButton_ZR)) ==
                (HidNpadButton_L | HidNpadButton_R | HidNpadButton_ZL | HidNpadButton_ZR)) {
            risky_prompt = false;
            do_explicit_fault();
        }
        printf("\x1b[2J\x1b[H");
        printf("Switch Memory Probe v1 | UP/DOWN scroll | Y recheck | + exit\n");
        printf("Report: %s\n", saved_path[0] ? saved_path : "SD write unavailable");
        printf("Status: SAFE checks completed; no intended page faults.\n");
        if (!risky_prompt)
            printf("X: OPTIONAL dangerous fault test (not needed for safe checks).\n");
        else
            printf("DANGER: may terminate app! HOLD L+R+ZL+ZR, press A to fault. X cancel.\n");
        printf("-----------------------------------------------------------------------\n");
        for (size_t i = (size_t)scroll; i < line_count && i < (size_t)(scroll + SHOW_LINES); ++i)
            printf("%s\n", lines[i]);
        printf("-----------------------------------------------------------------------\n");
        printf("Lines %d-%d / %zu\n", scroll + 1, scroll + SHOW_LINES, line_count);
        consoleUpdate(NULL);
    }
    consoleExit(NULL);
    return 0;
}
