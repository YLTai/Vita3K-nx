// Tests the real observer, not a stand-in for its ownership/coherence logic.
// Host execution does not validate Dynarmic native code or GPU visibility.
// SPDX-License-Identifier: GPL-2.0-or-later
#include <util/switch_write_watch.h>
#include <atomic>
#include <cassert>
#include <cstdlib>
#include <thread>

using namespace switch_validation::watch;
struct Fixture {
    std::unique_ptr<Page[]> ptes{new Page[page_count]{}};
    std::unique_ptr<uint8_t, decltype(&std::free)> bytes{
        static_cast<uint8_t *>(std::aligned_alloc(page_size, page_size * 8)), &std::free};
    Table table;
    Page *write_pt = nullptr;
    Fixture() {
        assert(bytes); std::memset(bytes.get(), 0xa5, page_size * 8);
        map(0x20, 0); map(0x21, 2); map(0x22, 4); map(0x40, 0);
        write_pt = table.bind(ptes.get()); assert(write_pt);
        assert(table.bind(ptes.get()) == write_pt);
    }
    void map(uint32_t page, uint32_t backing) {
        table.publish(ptes.get(), page, reinterpret_cast<Page>(
            reinterpret_cast<uintptr_t>(bytes.get() + backing * page_size) - uint64_t(page) * page_size));
    }
    Token observe(uint32_t guest, uint32_t backing, uint64_t hash, size_t size = 64) {
        auto token = table.before(ptes.get(), guest, bytes.get() + backing * page_size, size);
        table.after(token, hash); return token;
    }
    void collect() { table.phase(0, false, true); observe(0x20000, 0, 10); observe(0x21000, 2, 20); }
    void arm() { table.phase(1, true); observe(0x20000, 0, 10); observe(0x21000, 2, 20); }
    uint32_t word(unsigned page, unsigned off = 0) {
        uint32_t v; std::memcpy(&v, bytes.get() + page * page_size + off, 4); return v;
    }
};
int main() {
    {
        Fixture f; f.collect(); f.arm();
        assert(!f.write_pt[0x20] && !f.write_pt[0x21] && !f.write_pt[0x40]);
        assert(f.write_pt[0x22] == f.ptes[0x22]);
        uint32_t value = 0x11223344;
        assert(f.table.write(f.ptes.get(), 0x40000, &value, 4) == Access::Complete);
        assert(f.word(0) == value && f.write_pt[0x20] && f.write_pt[0x40]);
        f.observe(0x20000, 0, 11);
        auto s = f.table.snapshot();
        assert(s.phases[1].armed_pages == 2 && s.phases[1].armed_aliases == 3);
        assert(s.phases[1].first_write_pages == 1 && s.phases[1].changed_cpu_seen == 1);
        f.table.write(f.ptes.get(), 0x20004, &value, 4);
        assert(f.table.snapshot().phases[1].first_write_pages == 1);
        // A host/HLE-like store bypasses the JIT observer. Never hides its Hash.
        std::memcpy(f.bytes.get() + 2 * page_size, &value, 4);
        f.observe(0x21000, 2, 21);
        assert(f.table.snapshot().phases[1].changed_unobserved == 1);
        f.table.phase(2, true); // Explicit new observation epoch (not a safepoint proof).
        f.observe(0x20000, 0, 12); // First new-epoch Hash establishes a new baseline.
        assert(f.table.snapshot().phases[2].changed == 0);
        f.table.phase(3, false);
        assert(f.write_pt[0x20] && f.write_pt[0x21] && f.write_pt[0x40]);
    }
    {
        Fixture f; f.collect(); f.arm();
        uint64_t value = 0x0102030405060708ull;
        assert(f.table.write(f.ptes.get(), 0x20ffc, &value, 8) == Access::Complete);
        uint64_t read = 0;
        assert(f.table.read(f.ptes.get(), 0x20ffc, &read, 8) == Access::Complete && read == value);
        for (unsigned i = page_size; i < 2 * page_size; ++i) assert(f.bytes.get()[i] == 0xa5);
        assert(f.table.snapshot().phases[1].first_write_pages == 2);
        const uint32_t previous = f.word(4, page_size - 4);
        assert(f.table.write(f.ptes.get(), 0x22ffc, &value, 8) == Access::Invalid);
        assert(f.word(4, page_size - 4) == previous); // No partial write into valid first page.
        assert(f.table.write(f.ptes.get(), 0xfffffffc, &value, 8) == Access::Invalid);
        assert(f.table.write(f.ptes.get(), 0, &value, 8) == Access::Invalid);
        assert(f.table.snapshot().phases[1].invalid_accesses == 3);
    }
    {
        Fixture f; f.collect(); f.arm();
        bool success = true;
        assert(f.table.exclusive<uint32_t>(f.ptes.get(), 0x20000, 99, 0, success) == Access::Complete && !success);
        assert(!f.write_pt[0x20] && f.table.snapshot().phases[1].first_write_pages == 0);
        assert(f.table.exclusive<uint32_t>(f.ptes.get(), 0x20000, 99, 0xa5a5a5a5u, success) == Access::Complete && success);
        assert(f.word(0) == 99 && f.write_pt[0x40]);
        assert(f.table.exclusive<uint64_t>(f.ptes.get(), 0x20002, 9, 0, success) == Access::Invalid);
    }
    {
        Fixture f; f.collect(); f.arm();
        const auto ticket = f.table.before(f.ptes.get(), 0x20000, f.bytes.get(), 64);
        { auto guard = f.table.hold(); f.map(0x20, 6); }
        assert(f.write_pt[0x20] == f.ptes[0x20]); // New backing not left pointing at old memory.
        f.table.after(ticket, 100); // Mid-Hash remap cannot yield an unobserved-CPU accusation.
        assert(f.table.snapshot().phases[1].changed_unobserved == 0);
        f.map(0x50, 2); assert(!f.write_pt[0x50]); // New alias of an armed backing inherits watch.
        f.table.publish(f.ptes.get(), 0x50, nullptr); assert(!f.write_pt[0x50]);
        uint32_t v = 33;
        assert(f.table.write(f.ptes.get(), 0x50000, &v, 4) == Access::Invalid);
        f.table.detach(f.ptes.get());
        assert(f.table.write(f.ptes.get(), 0x20000, &v, 4) == Access::Unhandled);
        f.table.reset();
    }
    {
        Fixture f; f.collect(); f.arm();
        f.observe(0x22000, 4, 1); f.observe(0x22000, 4, 2);
        auto s = f.table.snapshot();
        assert(s.late_resources == 1 && s.phases[1].changed_uncovered == 1 && s.phases[1].changed_unobserved == 0);
        f.table.phase(2, true); f.observe(0x22000, 4, 2); f.observe(0x22000, 4, 3);
        assert(f.table.snapshot().phases[2].changed_unobserved == 1);
        // Huge or overflowed ranges never dereference host memory and cannot
        // fill the registry with half-registered groups.
        auto bad = f.table.before(f.ptes.get(), 0x20000, f.bytes.get(), uint64_t{1} << 63);
        assert(bad.resource == no_index && f.table.snapshot().backing_pages == 3);
        assert(f.table.snapshot().capacity_limited);
        f.table.phase(no_index, false); assert(f.write_pt[0x20] && f.write_pt[0x21]);
    }
    {
        Fixture f; f.collect();
        std::atomic<bool> start{false};
        auto writer = [&](uint32_t address) {
            while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
            for (uint32_t v = 0; v < 2000; ++v)
                assert(f.table.write(f.ptes.get(), address, &v, 4) == Access::Complete);
        };
        std::thread one(writer, 0x20000), two(writer, 0x21000);
        start.store(true, std::memory_order_release);
        for (unsigned n = 0; n < 20; ++n) {
            f.table.phase(1, true); auto s = f.table.snapshot(); assert(!s.failed);
            f.observe(0x20000, 0, n); f.table.phase(0, false);
        }
        one.join(); two.join();
        assert(f.word(0) == 1999 && f.word(2) == 1999);
        // This exercises metadata locking, NOT native stale-PTE revocation.
    }
    {
        Fixture f; f.collect(); f.arm();
        for (uint64_t h = 1; h < 300; ++h) f.observe(0x21000, 2, h);
        auto s = f.table.snapshot();
        assert(s.event_count == 128 && s.events_dropped > 0 && s.phases[1].changed_unobserved > 128);
    }
}
