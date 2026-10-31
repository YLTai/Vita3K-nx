#include <util/switch_write_watch.h>
#include <util/resource_hash_reuse.h>
#include <util/resource_write_ledger.h>
#include <util/observer_reuse_bridge.h>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#ifdef REUSE_TEST_PINNED_XXHASH
#define XXH_INLINE_ALL
#include <xxhash.h>
#else
extern "C" uint64_t XXH3_64bits(const void*, size_t);
#endif
using namespace switch_validation;
int main() {
    epoch::Domain domain;
    watch::Table table; table.set_epoch_domain(&domain);
    auto raw = std::make_unique<uint8_t*[]>(watch::page_count);
    auto *data = static_cast<uint8_t*>(std::aligned_alloc(4096, 8192)); assert(data);
    std::memset(data, 0, 8192);
    // Two Guest aliases share the same actual host backing.
    raw[2] = reinterpret_cast<uint8_t*>(uintptr_t(data) - 8192);
    raw[4] = reinterpret_cast<uint8_t*>(uintptr_t(data) - 16384);
    raw[3] = raw[2]; // adjacent palette page uses the same additive table bias
    raw[5] = raw[4];
    assert(table.bind(raw.get()));
    table.phase(0, false, true);
    table.before(raw.get(), 8192, data, 4096, data + 4096, 1024);
    table.phase(1, true, false, true);
    reuse::WriteLedger<8, 8> external;
    auto resource = external.add({uintptr_t(data), 4096}, {uintptr_t(data + 4096), 1024});
    // Explicit HOST TEST closure. NOT a production writer-coverage grant.
    uint32_t closed = reuse::CompleteCoverage;
    reuse::Input input{uintptr_t(data), uintptr_t(data + 4096), 4096, 1024, 1};
    auto snapshot = [&]() noexcept {
        auto token = table.before(raw.get(), 8192, data, 4096, data + 4096, 1024);
        return reuse::observer_evidence(table, token, external.stamp(resource), closed);
    };
    uint64_t actual_hash_calls = 0;
    auto full = [&]() {
        auto token = table.before(raw.get(), 8192, data, 4096, data + 4096, 1024);
        ++actual_hash_calls;
        auto hash = XXH3_64bits(data, 4096) ^ XXH3_64bits(data + 4096, 1024);
        table.after(token, hash); return hash;
    };
    auto cache = std::make_unique<reuse::HashCache<64>>();
    cache->begin(); cache->phase(0, reuse::Mode::Reference);
    assert(snapshot().rejection() == reuse::Reason::None);
    auto expected = cache->evaluate(input, snapshot, full);
    cache->phase(1, reuse::Mode::Audit);
    assert(cache->evaluate(input, snapshot, full) == expected);
    cache->phase(2, reuse::Mode::Performance);
    for (int i = 0; i < 512; ++i) assert(cache->evaluate(input, snapshot, full) == expected);
    assert(actual_hash_calls == 2);
    // Use the actual production observer callback path through the second alias.
    uint32_t value = 77;
    assert(table.write(raw.get(), 16384, &value, sizeof(value)) == watch::Access::Complete);
    expected = cache->evaluate(input, snapshot, full); assert(actual_hash_calls == 3);
    assert(expected == (XXH3_64bits(data,4096) ^ XXH3_64bits(data+4096,1024)));
    // Actual independently scoped host output (not a manually incremented proof).
    auto output = domain.begin_output({data+4096,4}, 0, true);
    std::memcpy(data+4096, &value, sizeof(value));
    assert(snapshot().host_busy);
    cache->evaluate(input, snapshot, full); assert(actual_hash_calls == 4);
    domain.end_output(output);
    cache->evaluate(input, snapshot, full); assert(actual_hash_calls == 5);
    // A tracked busy interval needs a fresh stable full Hash, not another
    // user-visible audit phase. Complete coverage/lifetime never disappeared.
    cache->evaluate(input, snapshot, full); assert(actual_hash_calls == 5);
    cache->phase(1, reuse::Mode::Audit);
    cache->evaluate(input, snapshot, full); assert(actual_hash_calls == 6);
    cache->phase(2, reuse::Mode::Performance);
    cache->evaluate(input, snapshot, full); assert(actual_hash_calls == 6);
    // Global unknown synchronous HLE activity is not declared read-only.
    { epoch::HostLease unknown(&domain, 99);
      assert(snapshot().unknown_busy);
      cache->evaluate(input, snapshot, full); assert(actual_hash_calls == 7); }
    cache->evaluate(input, snapshot, full); assert(actual_hash_calls == 8);
    cache->phase(1, reuse::Mode::Audit);cache->evaluate(input,snapshot,full);
    cache->phase(2, reuse::Mode::Performance);
    // Pending external GPU writes block only overlapping inputs. No fake
    // device execution here: exercise begin -> explicit visible completion.
    auto gpu = external.begin(reuse::Writer::Gpu, {uintptr_t(data), 4});
    assert(snapshot().gpu_busy);
    cache->evaluate(input,snapshot,full);
    std::memcpy(data, &value, sizeof(value));
    assert(external.finish(gpu, reuse::Completion::GpuHostVisible));
    auto before = actual_hash_calls; cache->evaluate(input,snapshot,full);
    assert(actual_hash_calls == before+1);
    // Missing async producer coverage never gains authority from old counters.
    closed &= ~reuse::AsynchronousHost;
    assert(snapshot().rejection() == reuse::Reason::Unsupported);
    before = actual_hash_calls;cache->evaluate(input,snapshot,full);
    assert(actual_hash_calls == before+1);
    closed |= reuse::AsynchronousHost;
    // Real mapping invalidation makes the persistent-coverage adapter reject.
    auto t = table.before(raw.get(),8192,data,4096,data+4096,1024);
    table.publish(raw.get(), 2, nullptr);
    assert(!table.fully_watched(t));
    assert(!snapshot().valid);
    table.phase(watch::no_index,false); table.detach(raw.get()); std::free(data);
    assert(!cache->report().failed);
    std::cout << "PASS: actual expanded Table/Domain/API -> new reuse bridge + real XXH3; "
        "512 skipped byte scans, Guest-alias callback invalidation, concrete HLE busy/end, "
        "unknown HLE, scoped GPU-ticket adapter, missing async coverage and remap fallback. "
        "No native A64/Horizon/Vulkan execution or live coverage grant.\n";
}
