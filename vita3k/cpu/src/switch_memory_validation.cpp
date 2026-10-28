// Isolated Switch Dynarmic memory validation and paired store microbenchmarks.
// SPDX-License-Identifier: GPL-2.0-or-later
#include <cpu/switch_memory_validation.h>
#include <util/switch_memory_validation.h>

#if defined(__SWITCH__) && defined(VITA3K_SWITCH_TRACELAB)

#include <dynarmic/interface/A32/a32.h>
#include <dynarmic/interface/exclusive_monitor.h>

#include <switch.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <initializer_list>
#include <malloc.h>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <vector>

namespace {

constexpr uint32_t kPageSize = 4096;
constexpr uint32_t kCodeBase = 0x1000;
constexpr uint32_t kDataBase = 0x20000;
constexpr uint32_t kAliasBase = 0x60000;
constexpr uint32_t kInvalidBase = 0x90000;
constexpr uint32_t kDataPages = 4;
constexpr uint32_t kStopSvc = 0x51;
constexpr uint32_t kSvc = 0xef000000 | kStopSvc;
constexpr uint32_t kBenchStores = 262144;
constexpr uint32_t kWarmupStores = kDataPages * kPageSize / 4;
constexpr uint32_t kTestValue = 0x89abcdef;

using PageTable = std::array<uint8_t *, Dynarmic::A32::UserConfig::NUM_PAGE_TABLE_ENTRIES>;

enum class TableMode { Original, Mirror, FirstWrite };

const char *mode_name(TableMode mode) {
    switch (mode) {
    case TableMode::Original: return "original_table";
    case TableMode::Mirror: return "mirror_write_table";
    case TableMode::FirstWrite: return "first_write_table";
    }
    return "unknown";
}

struct FreeBytes {
    void operator()(uint8_t *ptr) const { std::free(ptr); }
};

struct Mapping {
    uint32_t guest_page;
    uint32_t backing;
};

// Only this isolated fixture may reopen write entries in a callback. There is
// exactly one executing JIT and no HLE/GPU/other CPU writer. This intentionally
// does not claim to implement production epoch/rearm synchronization.
class MemoryFixture final : public Dynarmic::A32::UserCallbacks {
public:
    std::unique_ptr<PageTable> read_table{new PageTable{}};
    std::unique_ptr<PageTable> write_table{new PageTable{}};
    std::unique_ptr<uint8_t, FreeBytes> bytes{
        static_cast<uint8_t *>(memalign(kPageSize, kDataPages * 2 * kPageSize))};
    std::array<Mapping, kDataPages + 1> mappings{};
    Dynarmic::ExclusiveMonitor monitor{1};
    Dynarmic::A32::Jit *active_jit = nullptr;
    uint64_t writes = 0;
    uint64_t reads = 0;
    uint64_t invalid_writes = 0;
    uint64_t invalid_reads = 0;
    uint64_t first_writes = 0;
    uint64_t stopped = 0;
    uint32_t dirty_mask = 0;
    bool unexpected = false;

    uint32_t store8 = 0, store16 = 0, store32 = 0, store64 = 0;
    uint32_t load32 = 0, alias_store = 0, bench = 0;

    MemoryFixture() {
        if (!bytes)
            throw std::bad_alloc();
        for (uint32_t i = 0; i < kDataPages; ++i)
            mappings[i] = {kDataBase / kPageSize + i, i};
        mappings.back() = {kAliasBase / kPageSize, 0};
        for (const auto &mapping : mappings) {
            // Odd host pages are untouched sentinels. Consecutive guest pages
            // therefore have deliberately non-contiguous host backing.
            const uintptr_t host = reinterpret_cast<uintptr_t>(backing(mapping.backing));
            (*read_table)[mapping.guest_page] = reinterpret_cast<uint8_t *>(
                host - uintptr_t(mapping.guest_page) * kPageSize);
        }
        // Fixed ARMv7 A32 encodings. Instruction fetch uses MemoryReadCode,
        // never a guessed executable host pointer.
        store8 = append({0xe5c01000, kSvc}); // strb r1, [r0]
        store16 = append({0xe1c010b0, kSvc}); // strh r1, [r0]
        store32 = append({0xe5801000, kSvc}); // str r1, [r0]
        store64 = append({0xe1c020f0, kSvc}); // strd r2, r3, [r0]
        load32 = append({0xe5904000, kSvc}); // ldr r4, [r0]
        alias_store = append({0xe5801000, 0xe5832000, kSvc}); // str r1,[r0]; str r2,[r3]
        bench = append({
            0xe4801004, // str r1, [r0], #4
            0xe1500003, // cmp r0, r3
            0x01a00004, // moveq r0, r4
            0xe2522001, // subs r2, r2, #1
            0x1afffffa, // bne to the store (PC-relative -24 bytes)
            kSvc,
        });
        reset_bytes();
        arm(0);
    }

    uint8_t *backing(uint32_t id) const { return bytes.get() + id * 2 * kPageSize; }

    void reset_bytes() { std::memset(bytes.get(), 0xa5, kDataPages * 2 * kPageSize); }

    void reset_events() {
        writes = reads = invalid_writes = invalid_reads = first_writes = stopped = 0;
        unexpected = false;
    }

    bool arm(uint32_t backing_mask) {
        // Every caller is between Run()/Step() calls. Never infer that an
        // atomic PTE store alone can revoke a pointer already loaded by a JIT.
        if (active_jit && active_jit->IsExecuting())
            return false;
        watched_mask = backing_mask;
        dirty_mask = 0;
        for (const auto &mapping : mappings)
            (*write_table)[mapping.guest_page] = (backing_mask & (1u << mapping.backing))
                ? nullptr : (*read_table)[mapping.guest_page];
        return true;
    }

    void prepare(Dynarmic::A32::Jit &jit, uint32_t pc, uint32_t address = kDataBase + 128) {
        active_jit = &jit;
        jit.ClearHalt(Dynarmic::HaltReason::UserDefined1 | Dynarmic::HaltReason::Step);
        jit.Reset();
        jit.SetCpsr(0x1d0); // A32 user mode, little endian, interrupts masked.
        jit.Regs()[0] = address;
        jit.Regs()[1] = kTestValue;
        jit.Regs()[2] = 0x76543210;
        jit.Regs()[3] = 0xfedcba98;
        jit.Regs()[15] = pc;
    }

    bool clean_stop() const { return stopped != 0 && !unexpected; }

    bool matches(uint32_t address, uint64_t value, uint32_t size) const {
        for (uint32_t i = 0; i < size; ++i) {
            const auto *p = pointer(uint64_t(address) + i);
            if (!p || *p != uint8_t(value >> (8 * i)))
                return false;
        }
        return true;
    }

    bool guards_intact() const {
        for (uint32_t i = 0; i < kDataPages; ++i) {
            const auto *guard = backing(i) + kPageSize;
            if (!std::all_of(guard, guard + kPageSize, [](uint8_t v) { return v == 0xa5; }))
                return false;
        }
        return true;
    }

    bool bench_contents_match() const {
        for (uint32_t p = 0; p < kDataPages; ++p)
            for (uint32_t off = 0; off < kPageSize; off += 4)
                if (!matches(kDataBase + p * kPageSize + off, kTestValue, 4))
                    return false;
        return guards_intact();
    }

    std::optional<uint32_t> MemoryReadCode(uint32_t address) override {
        if (address < kCodeBase || (address & 3) || (address - kCodeBase) / 4 >= code.size())
            return std::nullopt;
        return code[(address - kCodeBase) / 4];
    }

    uint8_t MemoryRead8(uint32_t address) override { return read<uint8_t>(address); }
    uint16_t MemoryRead16(uint32_t address) override { return read<uint16_t>(address); }
    uint32_t MemoryRead32(uint32_t address) override { return read<uint32_t>(address); }
    uint64_t MemoryRead64(uint32_t address) override { return read<uint64_t>(address); }
    void MemoryWrite8(uint32_t address, uint8_t value) override { write(address, value); }
    void MemoryWrite16(uint32_t address, uint16_t value) override { write(address, value); }
    void MemoryWrite32(uint32_t address, uint32_t value) override { write(address, value); }
    void MemoryWrite64(uint32_t address, uint64_t value) override { write(address, value); }

    void CallSVC(uint32_t svc) override {
        if (svc == kStopSvc)
            ++stopped;
        else
            unexpected = true;
        active_jit->HaltExecution();
    }

    void InterpreterFallback(uint32_t, size_t) override { abort_test(); }
    void ExceptionRaised(uint32_t, Dynarmic::A32::Exception) override { abort_test(); }
    void AddTicks(uint64_t) override {}
    uint64_t GetTicksRemaining() override { return 1ull << 60; }

private:
    std::vector<uint32_t> code;
    uint32_t watched_mask = 0;

    uint32_t append(std::initializer_list<uint32_t> instructions) {
        const uint32_t pc = kCodeBase + static_cast<uint32_t>(code.size()) * 4;
        code.insert(code.end(), instructions);
        return pc;
    }

    const Mapping *mapping_for(uint64_t address) const {
        if (address >= (uint64_t{1} << 32))
            return nullptr;
        for (const auto &mapping : mappings)
            if (mapping.guest_page == address / kPageSize)
                return &mapping;
        return nullptr;
    }

    uint8_t *pointer(uint64_t address) const {
        const auto *mapping = mapping_for(address);
        return mapping ? backing(mapping->backing) + address % kPageSize : nullptr;
    }

    bool valid(uint32_t address, size_t size) const {
        // Validate the complete extent before writing anything. An invalid
        // mapping is an ordinary callback result, never a native fault probe.
        for (size_t i = 0; i < size; ++i)
            if (!pointer(uint64_t(address) + i))
                return false;
        return true;
    }

    template <typename T>
    T read(uint32_t address) {
        ++reads;
        if (!valid(address, sizeof(T))) {
            ++invalid_reads;
            return 0;
        }
        uint64_t result = 0;
        for (size_t i = 0; i < sizeof(T); ++i)
            result |= uint64_t(*pointer(uint64_t(address) + i)) << (8 * i);
        return static_cast<T>(result);
    }

    template <typename T>
    void write(uint32_t address, T value) {
        ++writes;
        if (!valid(address, sizeof(T))) {
            ++invalid_writes;
            return;
        }
        uint32_t touched = 0;
        for (size_t i = 0; i < sizeof(T); ++i) {
            const auto *mapping = mapping_for(uint64_t(address) + i);
            touched |= 1u << mapping->backing;
            *pointer(uint64_t(address) + i) = uint8_t(uint64_t(value) >> (8 * i));
        }
        for (uint32_t id = 0; id < kDataPages; ++id) {
            const uint32_t bit = 1u << id;
            if ((touched & watched_mask & bit) && !(dirty_mask & bit)) {
                dirty_mask |= bit;
                ++first_writes;
            }
        }
        // All guest aliases of the same physical test backing reopen together.
        // There is no renderer racing this commit in this single-writer suite.
        for (const auto &mapping : mappings)
            if (touched & (1u << mapping.backing))
                (*write_table)[mapping.guest_page] = (*read_table)[mapping.guest_page];
    }

    void abort_test() {
        unexpected = true;
        if (active_jit)
            active_jit->HaltExecution();
    }
};

Dynarmic::A32::UserConfig config_for(MemoryFixture &memory, TableMode mode, bool boundary_checks) {
    Dynarmic::A32::UserConfig config{};
    config.callbacks = &memory;
    config.arch_version = Dynarmic::A32::ArchVersion::v7;
    config.page_table = memory.read_table.get();
    config.absolute_offset_page_table = true;
    if (mode != TableMode::Original)
        config.write_page_table = memory.write_table.get();
    config.global_monitor = &memory.monitor;
    config.processor_id = 0;
    config.optimizations = Dynarmic::all_safe_optimizations;
    config.enable_cycle_counting = false;
    config.code_cache_size = 8 * 1024 * 1024;
    if (boundary_checks) {
        config.detect_misaligned_access_via_page_table = 16 | 32 | 64;
        config.only_detect_misalignment_via_page_table_on_page_boundary = true;
    }
    // Probe pointers remain null even if the real title enables TraceLab's
    // old per-store counters. No instrumentation is emitted on the fast path.
    return config;
}

void report_check(const char *name, bool passed, const MemoryFixture &memory, const char *extra = "") {
    char detail[384];
    std::snprintf(detail, sizeof(detail),
        "write_callbacks=%llu read_callbacks=%llu first_dirty_pages=%llu dirty_mask=0x%x invalid_writes=%llu invalid_reads=%llu %s",
        static_cast<unsigned long long>(memory.writes), static_cast<unsigned long long>(memory.reads),
        static_cast<unsigned long long>(memory.first_writes), memory.dirty_mask,
        static_cast<unsigned long long>(memory.invalid_writes), static_cast<unsigned long long>(memory.invalid_reads), extra);
    switch_validation::record("jit_selftest", name, passed ? "pass" : "fail", detail);
}

void run_correctness(MemoryFixture &memory) {
    Dynarmic::A32::Jit jit(config_for(memory, TableMode::FirstWrite, true));
    memory.active_jit = &jit;

    const std::array<uint32_t, 4> stores{memory.store8, memory.store16, memory.store32, memory.store64};
    const std::array<const char *, 4> names{"first_write_8", "first_write_16", "first_write_32", "first_write_64"};
    for (size_t i = 0; i < stores.size(); ++i) {
        memory.reset_bytes();
        memory.reset_events();
        const bool armed = memory.arm(1);
        memory.prepare(jit, stores[i]);
        jit.Run();
        memory.prepare(jit, stores[i]);
        jit.Run();
        const uint64_t value = i == 3 ? 0xfedcba9876543210ull : kTestValue;
        report_check(names[i], armed && memory.clean_stop() && memory.stopped == 2 && memory.writes == 1
                && memory.first_writes == 1 && memory.matches(kDataBase + 128, value, uint32_t{1} << i)
                && memory.guards_intact(), memory, "two stores; only the first may call back");
    }

    memory.reset_bytes();
    memory.reset_events();
    std::memcpy(memory.backing(0) + 128, &kTestValue, sizeof(kTestValue));
    memory.arm(1);
    memory.prepare(jit, memory.load32);
    jit.Run();
    report_check("read_while_write_closed", memory.clean_stop() && jit.Regs()[4] == kTestValue
            && memory.reads == 0 && memory.writes == 0 && !(*memory.write_table)[kDataBase / kPageSize], memory);

    bool rearm_ok = true;
    for (uint32_t epoch = 0; epoch < 2; ++epoch) {
        memory.reset_events();
        rearm_ok &= memory.arm(1) && !jit.IsExecuting();
        memory.prepare(jit, memory.store32);
        jit.Regs()[1] = kTestValue + epoch;
        jit.Run();
        rearm_ok &= memory.clean_stop() && memory.writes == 1 && memory.first_writes == 1
            && memory.matches(kDataBase + 128, kTestValue + epoch, 4);
    }
    report_check("rearm_between_runs", rearm_ok, memory, "single writer; both rearm points are outside Run");

    memory.reset_bytes();
    memory.reset_events();
    memory.arm(1);
    memory.prepare(jit, memory.store32);
    jit.Step();
    report_check("step_initializes_write_table", !memory.unexpected && memory.writes == 1
            && memory.first_writes == 1 && memory.matches(kDataBase + 128, kTestValue, 4), memory);

    memory.reset_bytes();
    memory.reset_events();
    memory.arm(1);
    memory.prepare(jit, memory.alias_store, kAliasBase + 128);
    jit.Regs()[2] = 0x12345678;
    jit.Regs()[3] = kDataBase + 128;
    jit.Run();
    report_check("alias_first_write_reopens_both", memory.clean_stop() && memory.writes == 1
            && memory.first_writes == 1 && memory.matches(kAliasBase + 128, 0x12345678, 4)
            && memory.matches(kDataBase + 128, 0x12345678, 4), memory);

    const std::array<uint32_t, 3> masks{1, 2, 3};
    const std::array<const char *, 3> crossing_names{
        "cross32_first_page_watched", "cross32_second_page_watched", "cross32_both_pages_watched"};
    for (size_t i = 0; i < masks.size(); ++i) {
        memory.reset_bytes();
        memory.reset_events();
        memory.arm(masks[i]);
        const uint32_t address = kDataBase + kPageSize - 2;
        memory.prepare(jit, memory.store32, address);
        jit.Run();
        report_check(crossing_names[i], memory.clean_stop() && memory.writes == 1
                && memory.dirty_mask == masks[i] && memory.matches(address, kTestValue, 4)
                && memory.guards_intact(), memory, "real emitter boundary check; non-contiguous host pages");
    }

    memory.reset_bytes();
    memory.reset_events();
    memory.arm(2);
    const uint32_t strd_address = kDataBase + kPageSize - 4;
    memory.prepare(jit, memory.store64, strd_address);
    jit.Run();
    report_check("cross64_second_page_watched", memory.clean_stop() && memory.writes == 1
            && memory.dirty_mask == 2 && memory.matches(strd_address, 0xfedcba9876543210ull, 8)
            && memory.guards_intact(), memory);

    memory.reset_bytes();
    memory.reset_events();
    memory.arm(0);
    memory.prepare(jit, memory.store32, kInvalidBase);
    jit.Run();
    report_check("invalid_mapping_store", memory.clean_stop() && memory.writes == 1
            && memory.invalid_writes == 1 && memory.guards_intact(), memory, "null PTE handled by callback; no native fault");

    memory.reset_events();
    memory.prepare(jit, memory.load32, kInvalidBase);
    jit.Run();
    report_check("invalid_mapping_read", memory.clean_stop() && memory.reads == 1
            && memory.invalid_reads == 1 && jit.Regs()[4] == 0, memory);

    memory.reset_bytes();
    memory.reset_events();
    memory.arm(1u << (kDataPages - 1));
    const uint32_t invalid_tail = kDataBase + kDataPages * kPageSize - 2;
    memory.prepare(jit, memory.store32, invalid_tail);
    jit.Run();
    report_check("cross_to_invalid_page_is_noop", memory.clean_stop() && memory.writes == 1
            && memory.invalid_writes == 1 && memory.first_writes == 0
            && memory.matches(invalid_tail, 0xa5a5, 2) && memory.guards_intact(), memory);

    memory.active_jit = nullptr;
    switch_validation::record("jit_selftest", "concurrency_scope", "skip",
        "single writer only; no production HLE/GPU coherence, concurrent rearm, or exclusive-store claim");
}

void run_config_rejections(MemoryFixture &memory) {
    for (bool missing_read_table : {false, true}) {
        bool rejected = false;
        auto config = config_for(memory, TableMode::Mirror, true);
        if (missing_read_table)
            config.page_table = nullptr;
        else
            config.fastmem_pointer = uintptr_t{1}; // Must be rejected before code allocation/execution.
        try {
            Dynarmic::A32::Jit invalid(config);
        } catch (const std::invalid_argument &) {
            rejected = true;
        }
        switch_validation::record("jit_selftest", missing_read_table ? "reject_write_table_without_read_table" : "reject_write_table_with_fastmem",
            rejected ? "pass" : "fail", "constructor rejection only; invalid configuration is never Run");
    }
}

void prepare_bench(MemoryFixture &memory, Dynarmic::A32::Jit &jit, TableMode mode, uint32_t stores) {
    memory.reset_bytes();
    memory.reset_events();
    memory.arm(mode == TableMode::FirstWrite ? (1u << kDataPages) - 1 : 0);
    memory.prepare(jit, memory.bench, kDataBase);
    jit.Regs()[2] = stores;
    jit.Regs()[3] = kDataBase + kDataPages * kPageSize;
    jit.Regs()[4] = kDataBase;
}

void bench_sample(MemoryFixture &memory, TableMode mode, const char *comparison, size_t slot) {
    // Keep only one 8 MiB JIT slice live at a time. Destruction returns it to
    // the existing shared pool; this suite never shuts down a process pool.
    Dynarmic::A32::Jit jit(config_for(memory, mode, false));
    prepare_bench(memory, jit, mode, kWarmupStores);
    jit.Run(); // Compile every loop/exit path outside the timing interval.
    const bool warm_ok = memory.clean_stop() && jit.Regs()[2] == 0;
    prepare_bench(memory, jit, mode, kBenchStores);
    const uint64_t start = armGetSystemTick();
    jit.Run();
    const uint64_t elapsed = armGetSystemTick() - start;
    const uint64_t expected_callbacks = mode == TableMode::FirstWrite ? kDataPages : 0;
    const bool passed = warm_ok && elapsed > 0 && memory.clean_stop() && jit.Regs()[2] == 0
        && memory.writes == expected_callbacks && memory.reads == 0
        && memory.first_writes == expected_callbacks
        && memory.dirty_mask == (mode == TableMode::FirstWrite ? (1u << kDataPages) - 1 : 0)
        && memory.invalid_writes == 0 && memory.bench_contents_match();
    char name[96], detail[384];
    std::snprintf(name, sizeof(name), "%s_abba_%zu_%s", comparison, slot + 1, mode_name(mode));
    std::snprintf(detail, sizeof(detail),
        "mode=%s order=ABBA slot=%zu pages=%u store_bytes=4 callbacks=%llu expected_callbacks=%llu boundary_checks=off_aligned_workload_only cycle_counting=off compile_outside_timing=1 host_above_4GiB=%u",
        mode_name(mode), slot + 1, kDataPages, static_cast<unsigned long long>(memory.writes),
        static_cast<unsigned long long>(expected_callbacks), unsigned(reinterpret_cast<uintptr_t>(memory.bytes.get()) > 0xffffffffull));
    switch_validation::record("store_microbench", name, passed ? "pass" : "fail", detail, elapsed, kBenchStores);
    memory.active_jit = nullptr;
}

} // namespace

void run_jit_memory_validation() {
    if (!switch_validation::enabled())
        return;
    try {
        {
            MemoryFixture memory;
            run_config_rejections(memory);
            run_correctness(memory);
            for (TableMode treatment : {TableMode::Mirror, TableMode::FirstWrite}) {
                const std::array<TableMode, 4> order{TableMode::Original, treatment, treatment, TableMode::Original};
                for (size_t slot = 0; slot < order.size(); ++slot)
                    bench_sample(memory, order[slot], mode_name(treatment), slot);
            }
        }
        switch_validation::record("jit_selftest", "allocation_scope", "pass",
            "isolated 16 MiB tables released; one 8 MiB code slice live at most; no real Guest mappings changed; caller closes idle shared code pool");
    } catch (const std::bad_alloc &) {
        switch_validation::record("jit_selftest", "allocation", "skip",
            "insufficient isolated page-table/backing/JIT memory; production configuration unchanged");
    } catch (const std::exception &e) {
        switch_validation::record("jit_selftest", "exception", "fail", e.what());
    }
}

#else

void run_jit_memory_validation() {}

#endif
