#!/usr/bin/env python3
"""Execute production reuse/lease headers with real XXH3.
Source producers are explicit host adapters, NOT live JIT/Horizon/Vulkan proof.
"""
from pathlib import Path
import argparse
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
CPP = r'''
#include <util/resource_hash_reuse.h>
#include <util/resource_write_ledger.h>
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstring>
#include <iostream>
#include <memory>
#include <random>
#include <stdexcept>
#include <thread>
#include <vector>
#ifdef REUSE_TEST_PINNED_XXHASH
#define XXH_INLINE_ALL
#include <xxhash.h>
#else
extern "C" uint64_t XXH3_64bits(const void*, size_t);
#endif
using namespace switch_validation::reuse;

struct Source {
    std::array<uint8_t, 8192> pixels{};
    std::array<uint8_t, 1024> palette{};
    Evidence proof{11, 1, 1, 1, 0, 0, 0, 0, 0, CompleteCoverage,
        true, true, false, false, false, false, true};
    uint64_t calls = 0;
    Input input(bool pal = true) const {
        return {reinterpret_cast<uintptr_t>(pixels.data()), pal ? reinterpret_cast<uintptr_t>(palette.data()) : 0,
            pixels.size(), pal ? palette.size() : 0, 0x585848330001};
    }
    uint64_t digest(bool pal = true) { ++calls; return XXH3_64bits(pixels.data(), pixels.size())
        ^ (pal ? XXH3_64bits(palette.data(), palette.size()) : 0); }
};
static void prime(HashCache<64> &c, Source &s) {
    c.begin(); c.phase(0, Mode::Reference);
    auto snap = [&]() noexcept { return s.proof; };
    auto full = [&]() { return s.digest(); };
    c.evaluate(s.input(), snap, full);
    c.phase(1, Mode::Audit);
    c.evaluate(s.input(), snap, full);
    assert(c.report().total.audits == 1);
    c.phase(2, Mode::Performance);
}
void core_tests() {
    HashCache<64> c; Source s;
    prime(c, s);
    const auto initial_calls = s.calls;
    const auto expected = s.digest(); --s.calls;
    for (unsigned i = 0; i < 10000; ++i) {
        auto h = c.evaluate(s.input(), [&]() noexcept { return s.proof; }, [&]() -> uint64_t {
            throw std::logic_error("clean reuse must not invoke the byte-reading reference");
        });
        assert(h == expected);
    }
    assert(s.calls == initial_calls);
    auto report = c.report();
    assert(report.total.reused_calls == 10000);
    assert(report.total.reused_bytes == 10000 * (s.pixels.size() + s.palette.size()));
    assert(report.total.reference_calls == 2);
    // Same-value CPU write is a write event, not a content comparison shortcut.
    ++s.proof.cpu;
    c.evaluate(s.input(), [&]() noexcept { return s.proof; }, [&]() { return s.digest(); });
    assert(s.calls == initial_calls + 1);
    // Pixel and palette changes both force a reference before reuse.
    ++s.proof.cpu; s.pixels[4095] ^= 7; s.pixels[4096] ^= 19;
    auto h = c.evaluate(s.input(), [&]() noexcept { return s.proof; }, [&]() { return s.digest(); });
    assert(h == (XXH3_64bits(s.pixels.data(), s.pixels.size()) ^ XXH3_64bits(s.palette.data(), s.palette.size())));
    ++s.proof.host; s.palette.back() ^= 81;
    h = c.evaluate(s.input(), [&]() noexcept { return s.proof; }, [&]() { return s.digest(); });
    auto n = s.calls;
    assert(c.evaluate(s.input(), [&]() noexcept { return s.proof; }, [&]() { return s.digest(); }) == h);
    assert(s.calls == n);
    // Lifecycle and algorithm changes cannot inherit old audit qualification.
    ++s.proof.lifetime;
    c.evaluate(s.input(), [&]() noexcept { return s.proof; }, [&]() { return s.digest(); });
    n = s.calls;
    c.evaluate(s.input(), [&]() noexcept { return s.proof; }, [&]() { return s.digest(); });
    assert(s.calls == n + 1); // same address, different allocation lifetime
    c.phase(4, Mode::Audit);
    c.evaluate(s.input(), [&]() noexcept { return s.proof; }, [&]() { return s.digest(); });
    c.phase(3, Mode::Performance);
    Input different = s.input(); ++different.hash_domain;
    n = s.calls;
    c.evaluate(different, [&]() noexcept { return s.proof; }, [&]() { return s.digest(); });
    assert(s.calls == n + 1);
    // Every coverage bit and every in-flight condition is a real fallback.
    for (uint32_t bit = 1; bit <= MappingLifetime; bit <<= 1) {
        auto saved = s.proof; s.proof.coverage &= ~bit; n = s.calls;
        c.evaluate(s.input(), [&]() noexcept { return s.proof; }, [&]() { return s.digest(); });
        assert(s.calls == n + 1); s.proof = saved;
    }
    for (int which = 0; which < 7; ++which) {
        auto saved = s.proof;
        if (which == 0) s.proof.valid = false;
        if (which == 1) s.proof.publication_drained = false;
        if (which == 2) s.proof.host_busy = true;
        if (which == 3) s.proof.async_busy = true;
        if (which == 4) s.proof.gpu_busy = true;
        if (which == 5) s.proof.unknown_busy = true;
        if (which == 6) s.proof.gpu_host_visible = false;
        n = s.calls;
        c.evaluate(s.input(), [&]() noexcept { return s.proof; }, [&]() { return s.digest(); });
        assert(s.calls == n + 1); s.proof = saved;
    }
    // Observations cannot be reused merely because they occur every N frames.
    s.proof.coverage = 0;
    for (int i = 0; i < 256; ++i)
        c.evaluate(s.input(), [&]() noexcept { return s.proof; }, [&]() { return s.digest(); });
    assert(c.report().total.fallback[size_t(Reason::Unsupported)] >= 256);
}
void oracle_and_race_tests() {
    HashCache<64> c; Source s;
    prime(c, s); c.phase(4, Mode::Audit);
    // A deliberately untracked write must return reference and disable reuse.
    s.pixels[17] ^= 1;
    auto actual = s.digest(); --s.calls;
    auto got = c.evaluate(s.input(), [&]() noexcept { return s.proof; }, [&]() { return s.digest(); });
    auto r = c.report();
    assert(got == actual && r.failed && !r.enabled && r.total.mismatches == 1);
    assert(r.examples == 1 && r.first_examples[0].cached != r.first_examples[0].reference);
    c.phase(2, Mode::Performance);
    auto n = s.calls;
    c.evaluate(s.input(), [&]() noexcept { return s.proof; }, [&]() { return s.digest(); });
    assert(s.calls == n + 1);
    // A source version changed during a reference scan cannot seed a baseline.
    c.begin(); c.phase(0, Mode::Reference);
    c.evaluate(s.input(), [&]() noexcept { return s.proof; }, [&]() {
        ++s.proof.cpu; s.pixels[100] ^= 27; return s.digest();
    });
    assert(c.report().total.raced_hashes == 1);
    c.phase(1, Mode::Audit);
    c.evaluate(s.input(), [&]() noexcept { return s.proof; }, [&]() { return s.digest(); });
    assert(c.report().total.audits == 0);
    // A restart while Hash is outside the metadata mutex discards old publication.
    c.evaluate(s.input(), [&]() noexcept { return s.proof; }, [&]() {
        c.begin(); return s.digest();
    });
    c.phase(2, Mode::Performance); n = s.calls;
    c.evaluate(s.input(), [&]() noexcept { return s.proof; }, [&]() { return s.digest(); });
    assert(s.calls == n + 1);
    // Losing coverage revokes the cached baseline even when a defective source
    // adapter later restores precisely the same old epoch values.
    prime(c, s); s.proof.coverage &= ~AsynchronousHost;
    s.pixels[31] ^= 119;
    c.evaluate(s.input(), [&]() noexcept { return s.proof; }, [&]() { return s.digest(); });
    s.proof.coverage |= AsynchronousHost; n = s.calls;
    auto fresh = c.evaluate(s.input(), [&]() noexcept { return s.proof; }, [&]() { return s.digest(); });
    assert(s.calls == n + 1);
    assert(fresh == (XXH3_64bits(s.pixels.data(), s.pixels.size()) ^ XXH3_64bits(s.palette.data(), s.palette.size())));
    // Exceptions do not promote a stale digest, and disable the candidate.
    try { c.evaluate(s.input(), [&]() noexcept { return s.proof; }, []() -> uint64_t {
        throw std::runtime_error("injected read failure");
    }); assert(false); } catch (const std::runtime_error &) {}
    assert(c.report().failed && c.report().total.exceptions == 1);
    Input invalid = s.input(); invalid.pixel_bytes = UINT64_MAX;
    assert(!invalid.valid());
}
void seeded_mutations() {
    HashCache<64> c; Source s; prime(c, s);
    std::mt19937 random(0x56544b33);
    for (unsigned i = 0; i < 25000; ++i) {
        const auto op = random() % 12;
        if (op < 4) { ++s.proof.cpu; s.pixels[random() % s.pixels.size()] ^= uint8_t(random()); }
        else if (op == 4) { ++s.proof.host; s.palette[random() % s.palette.size()] ^= uint8_t(random()); }
        else if (op == 5) { ++s.proof.cpu; } // same value
        else if (op == 6) { ++s.proof.unknown; }
        else if (op == 7) { ++s.proof.gpu; }
        else if (op == 8) { ++s.proof.async; }
        if (i % 127 == 0) c.phase(1, Mode::Audit);
        else c.phase(2, Mode::Performance);
        const auto actual = XXH3_64bits(s.pixels.data(), s.pixels.size()) ^ XXH3_64bits(s.palette.data(), s.palette.size());
        const auto result = c.evaluate(s.input(), [&]() noexcept { return s.proof; }, [&]() { return s.digest(); });
        assert(result == actual && !c.report().failed);
    }
    assert(c.report().total.reused_calls > 1000);
}
void ledger_tests() {
    WriteLedger<16, 16> l;
    const auto a = l.add({0x1000, 4096}, {0x9000, 1024});
    const auto alias = l.add({0x1000, 4096});
    const auto b = l.add({0x2000, 4096});
    const auto initial = l.stamp(a);
    auto w = l.begin(Writer::Async, {0x9fff, 1}); // not palette
    assert(!l.stamp(a).async_busy); assert(l.finish(w, Completion::HostWriteFinished));
    w = l.begin(Writer::Async, {0x93ff, 1}); // palette's last byte
    assert(l.stamp(a).async_busy && !l.stamp(alias).async_busy);
    assert(l.finish(w, Completion::HostWriteFinished));
    assert(l.stamp(a).async == initial.async + 2);
    const auto w1 = l.begin(Writer::Gpu, {0x1fff, 2}); // crosses both resources
    const auto w2 = l.begin(Writer::Gpu, {0x1000, 4});
    assert(l.stamp(a).gpu_busy && l.stamp(b).gpu_busy && l.stamp(alias).gpu_busy);
    assert(l.finish(w2, Completion::GpuHostVisible));
    assert(l.stamp(a).gpu_busy); // out-of-order completion does not clear w1
    assert(l.finish(w1, Completion::GpuHostVisible));
    assert(!l.stamp(a).gpu_busy && !l.stamp(b).gpu_busy);
    // Register interest after the external producer has already started.
    auto p = l.begin(Writer::Async, {0xa000, 128});
    auto late = l.add({0xa010, 32}); assert(l.stamp(late).async_busy);
    assert(l.finish(p, Completion::HostWriteFinished)); assert(!l.stamp(late).async_busy);
    assert(l.remove(late)); auto next = l.add({0xa010, 32});
    assert(!l.stamp(late).valid && l.stamp(next).valid && next.generation != late.generation);
    // Unknown destination conservatively blocks every current and late interest.
    p = l.begin(Writer::Async, {}, true);
    assert(l.stamp(a).async_busy && l.stamp(next).async_busy);
    auto newer = l.add({0xf000, 32}); assert(l.stamp(newer).async_busy);
    assert(l.finish(p, Completion::HostWriteFinished));
    // Fence completion without host visibility must not generate an idle proof.
    p = l.begin(Writer::Gpu, {0x1000, 32});
    assert(!l.finish(p, Completion::HostWriteFinished));
    assert(l.failed() && !l.stamp(a).valid);
    WriteLedger<2, 1> capacity;
    auto r = capacity.add({0x1000, 64});
    capacity.begin(Writer::Host, {0x1000, 32});
    assert(!capacity.begin(Writer::Host, {0x1000, 32}));
    assert(capacity.failed() && !capacity.stamp(r).valid);
    WriteLedger<2, 2> stale;
    auto t = stale.begin(Writer::Host, {0x1000, 4});
    assert(stale.finish(t, Completion::HostWriteFinished));
    assert(!stale.finish(t, Completion::HostWriteFinished) && stale.failed());
}
void concurrency_and_plan() {
    WriteLedger<16, 16> l;
    auto r = l.add({0x1000, 4096});
    std::vector<std::thread> threads;
    for (unsigned thread = 0; thread < 4; ++thread) threads.emplace_back([&]() {
        for (unsigned i = 0; i < 1024; ++i) {
            auto t = l.begin(Writer::Async, {0x1000, 16});
            assert(t && l.stamp(r).async_busy);
            assert(l.finish(t, Completion::HostWriteFinished));
        }
    });
    for (auto &t : threads) t.join();
    auto s = l.stamp(r);
    assert(s.valid && !s.async_busy && s.async == 8192);
    Plan plan;
    for (unsigned phase = 0; phase < 6; ++phase) {
        assert(plan.phase == phase && !plan.done);
        plan.interval(10'000'000'000ULL, false);
        assert(!plan.ready_to_advance());
        for (unsigned i = 0; i < 40; ++i) plan.interval(100'000'000, true);
        assert(plan.ready_to_advance()); assert(plan.advance());
    }
    assert(plan.done);
    Plan slow;
    for (unsigned i = 0; i < 10; ++i) slow.interval(1'000'000'000, true);
    assert(slow.ready_to_advance() && slow.samples < 32);
}
int main() {
    core_tests(); oracle_and_race_tests(); seeded_mutations(); ledger_tests(); concurrency_and_plan();
    std::cout << "PASS: actual headers + XXH3; 10000 no-read reuses, 25000 seeded transitions, "
      "4096 concurrent output tickets, generation/alias/palette/lifetime/busy/fallback, "
      "untracked-write negative, completion+visibility and six-stage scheduling. "
      "Host adapters only; NOT live Guest/Horizon/Vulkan validation.\n";
}
'''

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--cxx', default=os.environ.get('CXX', 'c++'))
    p.add_argument('--sanitize', action='store_true')
    p.add_argument('--require-source', action='store_true', help='compile the actual expanded production observer as well')
    p.add_argument('--observer-include', type=Path, help='explicit reconstructed include root for local verification only')
    args = p.parse_args()
    with tempfile.TemporaryDirectory(prefix='v3k-reuse-') as d:
        d = Path(d); (d/'test.cpp').write_text(CPP)
        xxhash = ROOT/'external/xxHash/xxhash.h'
        hash_flags = ['-DREUSE_TEST_PINNED_XXHASH', '-I'+str(xxhash.parent)] if xxhash.exists() else ['-lxxhash']
        if args.require_source and not xxhash.exists():
            raise RuntimeError('full-checkout gate requires the pinned external/xxHash header')
        print('Hash backend: '+('repository pinned header' if xxhash.exists() else 'system libxxhash (not target pin)'), flush=True)
        cmd = [args.cxx, '-std=c++20', '-Wall', '-Wextra', '-Werror', '-pthread',
            '-I'+str(ROOT/'vita3k/util/include'), str(d/'test.cpp'), *hash_flags, '-o', str(d/'test')]
        cmd += ['-O1', '-g', '-fno-omit-frame-pointer', '-fsanitize=address,undefined'] if args.sanitize else ['-O2']
        subprocess.run(cmd, check=True, timeout=90)
        subprocess.run([str(d/'test')], check=True, timeout=45,
            env=dict(os.environ, ASAN_OPTIONS='detect_leaks=1:halt_on_error=1', UBSAN_OPTIONS='halt_on_error=1'))
        if args.require_source or args.observer_include:
            include = args.observer_include or ROOT/'vita3k/util/include'
            actual = include/'util/switch_write_watch.h'
            if 'bool fully_watched(const Token &t) const noexcept' not in actual.read_text():
                raise RuntimeError('missing applied source admission hook')
            fixture = ROOT/'scripts/tests/fixtures/resource_reuse_observer.cpp'
            build = [args.cxx, '-std=c++20', '-Wall', '-Wextra', '-Werror', '-pthread',
                '-I'+str(ROOT/'vita3k/util/include'), '-I'+str(include), str(fixture),
                *hash_flags, '-o', str(d/'observer')]
            build += ['-O1', '-g', '-fno-omit-frame-pointer', '-fsanitize=address,undefined'] if args.sanitize else ['-O2']
            subprocess.run(build, check=True, timeout=90)
            subprocess.run([str(d/'observer')], check=True, timeout=45,
                env=dict(os.environ, ASAN_OPTIONS='detect_leaks=1:halt_on_error=1', UBSAN_OPTIONS='halt_on_error=1'))

if __name__ == '__main__': main()
