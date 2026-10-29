// Recoverable write-fault admission is confined to registered private Heap pages.
// No Guest page, GPU allocation, exception-frame PC, or original stack is changed.
// SPDX-License-Identifier: GPL-2.0-or-later
#include <util/switch_heap_fault_probe.h>

#if defined(__SWITCH__) && defined(VITA3K_SWITCH_TRACELAB)
#include <util/heap_fault_policy.h>
#include <util/switch_memory_validation.h>
#include <switch.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <malloc.h>
#include <sys/stat.h>
#include <unistd.h>

namespace switch_validation {
namespace {
static_assert(sizeof(uintptr_t) == 8);
static_assert(std::atomic<uint32_t>::is_always_lock_free);
static_assert(std::atomic<uintptr_t>::is_always_lock_free);
static_assert(offsetof(ThreadExceptionFrameA64, elr_el1) == 88);
static_assert(offsetof(ThreadExceptionFrameA64, pstate) == 96);
static_assert(offsetof(ThreadExceptionFrameA64, esr) == 108);
static_assert(offsetof(ThreadExceptionFrameA64, far) == 112);
static_assert(sizeof(ThreadExceptionFrameA64) == 120);

struct Slot {
    // 0 unused, 1 being prepared, otherwise owning thread's TLS address.
    std::atomic<uintptr_t> owner{0};
    uintptr_t begin = 0, pc = 0;
    size_t size = 0;
    std::atomic<uint32_t> pending{0};
    std::atomic<uint32_t> entered{0};
    HeapFaultResult result{};
};
std::array<Slot, 4> slots;
std::atomic<bool> safe_to_chainload{true};
bool native_ok = false;
bool checkpoint_ok = false;
char checkpoint_path[256]{};
constexpr uint32_t bad_state = 0xf601;
uintptr_t owner_id() noexcept { return reinterpret_cast<uintptr_t>(armGetTls()); }
Slot *own_slot() noexcept {
    const auto owner = owner_id();
    for (auto &slot : slots)
        if (slot.owner.load(std::memory_order_acquire) == owner) return &slot;
    return nullptr;
}
bool writable_heap(uintptr_t begin, size_t size) noexcept {
    for (size_t off = 0; off < size; off += fault::page_size) {
        MemoryInfo info{}; u32 page_info = 0;
        if (R_FAILED(svcQueryMemory(&info, &page_info, begin + off))
            || (info.type & 0xff) != MemType_Heap || info.perm != Perm_Rw || info.attr != 0)
            return false;
    }
    return true;
}
void result(const char *name, bool pass, const HeapFaultResult &r, uint64_t ticks = 0,
    uint64_t iterations = 0) {
    char detail[320];
    std::snprintf(detail, sizeof(detail),
        "faults=%llu dirty=0x%x restored=%u rc=0x%x last_pc=0x%llx far=0x%llx handler_ticks=%llu; isolated Heap only",
        static_cast<unsigned long long>(r.faults), r.dirty_mask, unsigned(r.restored), r.last_rc,
        static_cast<unsigned long long>(r.last_pc), static_cast<unsigned long long>(r.last_far),
        static_cast<unsigned long long>(r.handler_ticks));
    record("heap_fault", name, pass ? "pass" : "fail", detail, ticks, iterations);
    native_ok &= pass;
}
using Store = void (*)(void *, uint64_t);
extern "C" {
void v3k_heap_store8(void *, uint64_t); void v3k_heap_store16(void *, uint64_t);
void v3k_heap_store32(void *, uint64_t); void v3k_heap_store64(void *, uint64_t);
void v3k_heap_store128(void *, uint64_t);
extern char v3k_heap_store8_pc[], v3k_heap_store16_pc[], v3k_heap_store32_pc[], v3k_heap_store64_pc[], v3k_heap_store128_pc[];
void v3k_heap_register_test(void *, void *);
extern char v3k_heap_register_test_pc[];
}
} // namespace

// Called while still in the kernel's user-exception context. No allocations,
// logging, stdio, locks, GPU operations, thread waits or C++ exceptions here.
extern "C" __attribute__((used, noinline, externally_visible)) uint32_t v3k_try_heap_fault(uint64_t, ThreadExceptionFrameA64 *frame) noexcept {
    if (!frame) return 0;
    auto *slot = own_slot();
    if (!slot) return 0;
    const fault::Evidence evidence{static_cast<uintptr_t>(frame->far),
        static_cast<uintptr_t>(frame->elr_el1), frame->esr, frame->pstate};
    const auto pending = slot->pending.load(std::memory_order_acquire);
    if (!fault::admissible(evidence, slot->begin, slot->size, slot->pc, pending, true)
        || slot->entered.load(std::memory_order_relaxed)) return 0;
    slot->entered.store(1, std::memory_order_relaxed);
    const uint64_t tick = armGetSystemTick();
    const uintptr_t page = evidence.far & ~(uintptr_t(fault::page_size) - 1);
    const uint32_t bit = 1u << ((page - slot->begin) / fault::page_size);
    const auto rc = svcSetMemoryPermission(reinterpret_cast<void *>(page), fault::page_size, Perm_Rw);
    slot->result.last_pc = evidence.pc;
    slot->result.last_far = evidence.far;
    slot->result.last_rc = rc;
    slot->result.handler_ticks += armGetSystemTick() - tick;
    if (R_SUCCEEDED(rc)) {
        ++slot->result.faults;
        slot->result.dirty_mask |= bit;
        slot->pending.store(pending & ~bit, std::memory_order_release);
    }
    slot->entered.store(0, std::memory_order_relaxed);
    // The frame is never modified: the kernel retries the original instruction.
    // Failure or an unrelated exception follows the stock libnx crash path.
    return R_SUCCEEDED(rc) ? 1 : 0;
}

bool heap_fault_arm(void *memory, size_t size, uintptr_t expected_pc) noexcept {
    if (!heap_fault_probe_enabled() || !checkpoint_ok || own_slot()) return false;
    const auto begin = reinterpret_cast<uintptr_t>(memory);
    if (!begin || !size || (begin | size) % fault::page_size || size > fault::max_pages * fault::page_size
        || begin + size < begin || !writable_heap(begin, size)) return false;
    for (auto &slot : slots) {
        uintptr_t empty = 0;
        if (!slot.owner.compare_exchange_strong(empty, 1, std::memory_order_acq_rel)) continue;
        slot.begin = begin; slot.size = size; slot.pc = expected_pc; slot.result = {};
        slot.entered.store(0, std::memory_order_relaxed);
        slot.pending.store((1u << (size / fault::page_size)) - 1, std::memory_order_relaxed);
        slot.owner.store(owner_id(), std::memory_order_release);
        const auto rc = svcSetMemoryPermission(memory, size, Perm_R);
        if (R_SUCCEEDED(rc)) return true;
        slot.result.last_rc = rc;
        // SVC failure is not assumed transactional. Verify RW before releasing.
        const auto restore = svcSetMemoryPermission(memory, size, Perm_Rw);
        if (R_FAILED(restore) || !writable_heap(begin, size)) safe_to_chainload.store(false);
        slot.owner.store(0, std::memory_order_release);
        return false;
    }
    return false;
}

HeapFaultResult heap_fault_disarm() noexcept {
    auto *slot = own_slot();
    if (!slot) { HeapFaultResult r; r.last_rc = bad_state; return r; }
    // Keep ownership until all pages are restored; caller has stopped test stores.
    const auto rc = svcSetMemoryPermission(reinterpret_cast<void *>(slot->begin), slot->size, Perm_Rw);
    auto out = slot->result;
    out.restored = R_SUCCEEDED(rc) && writable_heap(slot->begin, slot->size);
    if (R_FAILED(rc)) out.last_rc = rc;
    if (!out.restored) safe_to_chainload.store(false);
    slot->pending.store(0, std::memory_order_release);
    slot->owner.store(0, std::memory_order_release);
    return out;
}

bool heap_fault_safe_to_chainload() noexcept { return safe_to_chainload.load(); }
bool heap_fault_jit_allowed() { return native_ok && checkpoint_ok && heap_fault_safe_to_chainload(); }

bool heap_fault_checkpoint(const char *test, bool armed) noexcept {
    if (!checkpoint_ok) return false;
    FILE *file = std::fopen(checkpoint_path, "wb");
    if (!file) { checkpoint_ok = false; return false; }
    // Controlled names only. A partial/truncated marker blocks retry as well.
    bool ok = std::fprintf(file, "{\"schema\":1,\"state\":\"%s\",\"test\":\"%s\",\"scratch_only\":true}\n",
        armed ? "armed" : "returned", test) > 0;
    ok &= std::fflush(file) == 0;
    ok &= ::fsync(fileno(file)) == 0;
    ok &= std::fclose(file) == 0;
    checkpoint_ok &= ok;
    return ok;
}

void heap_fault_probe_finished() noexcept {
    if (heap_fault_probe_enabled()) (void)heap_fault_checkpoint("suite_finished", false);
}

HeapPermissionResult heap_permission_roundtrip(void *pointer, size_t size) noexcept {
    HeapPermissionResult out;
    const uintptr_t begin = reinterpret_cast<uintptr_t>(pointer);
    if (!begin || !size || (begin | size) % fault::page_size || begin + size < begin) {
        out.read_rc = bad_state; return out;
    }
    const auto has_permission = [&](uint32_t permission) {
        for (size_t off = 0; off < size; off += fault::page_size) {
            MemoryInfo info{}; u32 page_info = 0;
            if (R_FAILED(svcQueryMemory(&info, &page_info, begin + off)) || info.perm != permission)
                return false;
        }
        return true;
    };
    MemoryInfo info{}; u32 page_info = 0;
    out.query_before = R_SUCCEEDED(svcQueryMemory(&info, &page_info, begin)) && has_permission(Perm_Rw);
    out.before_type = info.type; out.before_attr = info.attr;
    if (!out.query_before) { out.read_rc = bad_state; return out; }
    out.read_rc = svcSetMemoryPermission(pointer, size, Perm_R);
    out.read_only = R_SUCCEEDED(out.read_rc) && has_permission(Perm_R);
    // Pinned memory can reject both calls while staying RW. Verify actual
    // permissions rather than treating a rejected no-op RW request as damage.
    if (!has_permission(Perm_Rw))
        out.restore_rc = svcSetMemoryPermission(pointer, size, Perm_Rw);
    out.writable_after = has_permission(Perm_Rw);
    return out;
}
void heap_fault_retain_allocation() noexcept { safe_to_chainload.store(false); }
void copy_heap_fault_checkpoint(const char *directory) noexcept {
    if (!directory || !checkpoint_path[0]) return;
    FILE *input = std::fopen(checkpoint_path, "rb");
    if (!input) return;
    char path[1024]; std::snprintf(path, sizeof(path), "%s/heap-fault-checkpoint.json", directory);
    FILE *output = std::fopen(path, "wb");
    char buffer[512]; size_t count;
    bool ok = output != nullptr;
    if (output) {
        while ((count = std::fread(buffer, 1, sizeof(buffer), input)) != 0)
            ok &= std::fwrite(buffer, 1, count, output) == count;
        ok &= std::ferror(input) == 0;
        ok &= std::fclose(output) == 0;
    }
    std::fclose(input);
    if (!ok) record("heap_fault", "checkpoint_export", "fail", "could not copy standalone checkpoint into TraceLab directory");
}

void run_heap_fault_probe() {
    if (!heap_fault_probe_enabled()) return;
    native_ok = false; checkpoint_ok = false;
    mkdir("sdmc:/switch/vita3k/logs", 0777);
#ifdef VITA3K_MEMORY_VALIDATION_REVISION
    const char *revision = VITA3K_MEMORY_VALIDATION_REVISION;
#else
    const char *revision = "development";
#endif
    std::snprintf(checkpoint_path, sizeof(checkpoint_path),
        "sdmc:/switch/vita3k/logs/heap-fault-%.12s.json", revision);
    if (FILE *old = std::fopen(checkpoint_path, "rb")) {
        char data[512]{}; (void)std::fread(data, 1, sizeof(data) - 1, old); std::fclose(old);
        if (!std::strstr(data, "\"state\":\"returned\"") || !std::strstr(data, "\"schema\":1")
            || !std::strstr(data, "\"scratch_only\":true}")) {
            record("heap_fault", "previous_abort", "fail", data);
            record("heap_fault", "auto_retry_blocked", "skip", "prior attempt did not return; preserve checkpoint and share it; no repeated fault loop");
            return;
        }
    }
    checkpoint_ok = true;
    if (!heap_fault_checkpoint("setup", false)) {
        record("heap_fault", "checkpoint", "fail", "cannot durably save the pre-fault marker; no fault attempted"); return;
    }
    const bool allowed = envIsSyscallHinted(0x02) && envIsSyscallHinted(0x28);
    if (!allowed) { record("heap_fault", "svc_hints", "fail", "permission/exception-return SVC not hinted; no fault attempted"); return; }
    auto *allocation = static_cast<uint8_t *>(memalign(fault::page_size, 6 * fault::page_size));
    if (!allocation) { record("heap_fault", "allocation", "fail", "private Heap allocation failed"); return; }
    std::memset(allocation, 0xa5, 6 * fault::page_size);
    uint8_t *const data = allocation + fault::page_size;
    native_ok = true;
    const std::array<Store, 5> stores{v3k_heap_store8, v3k_heap_store16, v3k_heap_store32, v3k_heap_store64, v3k_heap_store128};
    const std::array<uintptr_t, 5> pcs{reinterpret_cast<uintptr_t>(v3k_heap_store8_pc), reinterpret_cast<uintptr_t>(v3k_heap_store16_pc),
        reinterpret_cast<uintptr_t>(v3k_heap_store32_pc), reinterpret_cast<uintptr_t>(v3k_heap_store64_pc), reinterpret_cast<uintptr_t>(v3k_heap_store128_pc)};
    constexpr uint64_t value = 0x8172635445362718ull;
    for (size_t i = 0; i < stores.size() && native_ok; ++i) {
        char name[48]; std::snprintf(name, sizeof(name), "native_store_%zu", size_t{8} << i);
        if (!heap_fault_checkpoint(name, true) || !heap_fault_arm(data, fault::page_size, pcs[i])) {
            record("heap_fault", name, "fail", "could not arm verified scratch page"); native_ok = false; break;
        }
        stores[i](data + 64, value); stores[i](data + 64, value); // Second store must not fault again.
        const auto r = heap_fault_disarm();
        bool ok = r.restored && r.faults == 1 && r.last_pc == pcs[i] && r.dirty_mask == 1;
        if (r.restored) for (size_t b = 0; b < (size_t{1} << i); ++b) ok &= data[64 + b] == uint8_t(value >> (8 * (b % 8)));
        result(name, ok, r); if (!heap_fault_checkpoint(name, false)) native_ok = false;
    }
    if (native_ok) {
        alignas(16) std::array<uint8_t, 1600> registers{};
        if (heap_fault_checkpoint("register_context", true)
            && heap_fault_arm(data, fault::page_size, reinterpret_cast<uintptr_t>(v3k_heap_register_test_pc))) {
            v3k_heap_register_test(data + 64, registers.data());
            const auto r = heap_fault_disarm();
            uint64_t written = 0;
            if (r.restored) std::memcpy(&written, data + 64, sizeof(written));
            result("register_context", r.restored && r.faults == 1 && written == 0x5102
                && std::memcmp(registers.data(), registers.data() + 800, 800) == 0, r);
            (void)heap_fault_checkpoint("register_context", false);
        } else { record("heap_fault", "register_context", "fail", "arm/checkpoint failed"); native_ok = false; }
    }
    if (native_ok) {
        if (heap_fault_checkpoint("cross_page_128", true) && heap_fault_arm(data, 2 * fault::page_size, pcs[4])) {
            v3k_heap_store128(data + fault::page_size - 8, value);
            const auto r = heap_fault_disarm();
            bool ok = r.restored && r.dirty_mask == 3 && r.faults == 2;
            if (r.restored) for (size_t b = 0; b < 16; ++b) ok &= data[fault::page_size - 8 + b] == uint8_t(value >> (8 * (b % 8)));
            result("cross_page_128", ok, r); (void)heap_fault_checkpoint("cross_page_128", false);
        } else { record("heap_fault", "cross_page_128", "fail", "arm/checkpoint failed"); native_ok = false; }
    }
    if (native_ok) {
        if (heap_fault_checkpoint("host_memset", true) && heap_fault_arm(data, 4 * fault::page_size)) {
            // Exactly the raw host-pointer kind of write bypassing JIT callbacks.
            std::memset(data, 0x3c, 4 * fault::page_size);
            const auto r = heap_fault_disarm();
            result("host_memset", r.restored && r.faults == 4 && r.dirty_mask == 15
                && std::all_of(data, data + 4 * fault::page_size, [](uint8_t v) { return v == 0x3c; }), r);
            (void)heap_fault_checkpoint("host_memset", false);
        } else { record("heap_fault", "host_memset", "fail", "arm/checkpoint failed"); native_ok = false; }
    }
    if (native_ok && heap_fault_checkpoint("rearm_64", true)) {
        HeapFaultResult aggregate; aggregate.restored = true;
        const uint64_t start = armGetSystemTick();
        for (unsigned n = 0; n < 64; ++n) {
            if (!heap_fault_arm(data, fault::page_size, pcs[2])) { aggregate.restored = false; break; }
            v3k_heap_store32(data + 64, value + n);
            const auto r = heap_fault_disarm();
            aggregate.faults += r.faults; aggregate.handler_ticks += r.handler_ticks;
            aggregate.restored &= r.restored; aggregate.last_rc = r.last_rc;
            if (!r.restored || r.faults != 1) break;
        }
        result("rearm_64", aggregate.restored && aggregate.faults == 64, aggregate, armGetSystemTick() - start, 64);
        (void)heap_fault_checkpoint("rearm_64", false);
    }
    const bool safe = heap_fault_safe_to_chainload();
    const bool guards = safe && std::all_of(allocation, data, [](uint8_t v) { return v == 0xa5; })
        && std::all_of(data + 4 * fault::page_size, allocation + 6 * fault::page_size, [](uint8_t v) { return v == 0xa5; });
    record("heap_fault", "native_cleanup", safe && guards ? "pass" : "fail",
        safe ? "all scratch permissions RW; allocation guards checked; no Guest mapping changed" : "restore unverified: scratch retained; chainload prohibited");
    native_ok &= safe && guards && checkpoint_ok;
    if (safe) std::free(allocation); // Never return a still-protected allocation to malloc.
    record("heap_fault", "native_suite_complete", native_ok ? "pass" : "fail",
        "kernel retry at original PC, GPR/Q/SP/NZCV/FPCR/FPSR comparison, crossing, host memset and bounded rearm; no GPU/IPC or concurrent-writer claim");
}
} // namespace switch_validation
#endif
