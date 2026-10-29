// Tests real metadata logic. Native instructions are tested separately on device.
// SPDX-License-Identifier: GPL-2.0-or-later
#include <util/switch_write_watch.h>
#include <cassert>
#include <cstdlib>
using namespace switch_validation::watch;
struct Fixture {
    std::unique_ptr<Page[]> ptes{new Page[page_count]{}};
    std::unique_ptr<uint8_t, decltype(&std::free)> bytes{
        static_cast<uint8_t *>(std::aligned_alloc(page_size, page_size * 4)), &std::free};
    Table t;
    Page *writes = nullptr;
    Fixture() {
        assert(bytes); std::memset(bytes.get(), 0, page_size * 4);
        map(0x20,0); map(0x21,2); map(0x40,0);
        writes=t.bind(ptes.get()); assert(writes);
    }
    void map(uint32_t g,unsigned b) { t.publish(ptes.get(),g,reinterpret_cast<Page>(uintptr_t(bytes.get()+b*page_size)-uint64_t(g)*page_size)); }
    uint32_t value(unsigned off) const { uint32_t v; std::memcpy(&v,bytes.get()+off,4);return v; }
    Audit inspect(unsigned off) {
        auto x=t.before(ptes.get(),0x20000+off,bytes.get()+off,4);
        return t.after(x,value(off));
    }
    void poke(unsigned off,uint32_t v) { std::memcpy(bytes.get()+off,&v,4); }
    void arm() {t.phase(0,false,true);inspect(128);inspect(132);t.phase(1,true,false,true);inspect(128);inspect(132);}
    void store(uint32_t addr,uint32_t value) {assert(t.write(ptes.get(),addr,&value,4)==Access::Complete);}
};
int main() {
    { Fixture f; f.arm();
      f.map(0x20,0);assert(!f.writes[0x20]); // Unchanged PTE publication must keep guard.
      f.store(0x40080,1); assert(f.inspect(128).reason==HashReason::CpuWrite);
      f.store(0x20080,2); assert(f.inspect(128).reason==HashReason::CpuWrite);
      assert(!f.writes[0x20] && !f.writes[0x40]);
      assert(f.t.snapshot().phases[1].first_write_pages==1 && f.t.snapshot().phases[1].watched_stores==2);
      f.poke(128,3);assert(f.inspect(128).reason==HashReason::Unobserved); // Sticky-taint regression.
      f.store(0x20000,4);f.poke(132,5);assert(f.inspect(132).reason==HashReason::Unobserved);
      assert(f.t.snapshot().phases[1].page_only_stores==1);
      f.poke(128,6);f.t.host_written(f.bytes.get()+128,4,HostWriter::Memcpy);
      assert(f.inspect(128).reason==HashReason::HostNotice);
      f.poke(128,7);assert(f.inspect(128).reason==HashReason::Unobserved); // No sticky host notice.
      auto tok=f.t.before(f.ptes.get(),0x20080,f.bytes.get()+128,4);
      f.store(0x20080,8);assert(f.t.after(tok,8).reason==HashReason::Raced);
      assert(f.inspect(128).reason==HashReason::Baseline); // Raced read must not train.
      tok=f.t.before(f.ptes.get(),0x20080,f.bytes.get()+128,4);f.map(0x20,3);
      assert(f.t.after(tok,9).reason==HashReason::Raced);
      assert(f.t.snapshot().phases[1].discarded_tokens==1);
    }
    { Fixture f; f.arm(); // Actual cache's before value need not equal registry's last value.
      auto a=f.inspect(128);f.t.cache_decision(a,2,0x20080,4,99,0,true);
      auto s=f.t.snapshot();assert(s.cache.changes==1 && s.cache.reasons[size_t(CacheReason::DifferentBaseline)]==1);
      f.t.phase(2,true,false,true);a=f.inspect(128);
      f.t.cache_decision(a,2,0x20080,4,99,0,true);
      s=f.t.snapshot();assert(s.cache.reasons[size_t(CacheReason::FirstBaseline)]==1);
      auto stale=f.t.before(f.ptes.get(),0x20080,f.bytes.get()+128,4);
      f.t.phase(0,false,true);assert(!f.t.after(stale,0).valid); // Restart cannot accept old resource id.
      assert(f.t.snapshot().cache.changes==2); // Session ledger survives restart.
      for(unsigned i=0;i<150;++i)f.t.cache_decision({},2,0x20080,4,1,2,true);
      s=f.t.snapshot();assert(s.cache.changes==152 && s.cache.stored==64 && s.cache.dropped==88);
      uint64_t sum=0;for(auto n:s.cache.reasons)sum+=n;assert(sum==s.cache.changes);
    }
    { Fixture f; f.t.phase(0,false,true);
      // Pixel and separate palette ranges must independently advance evidence.
      auto observe=[&] {auto x=f.t.before(f.ptes.get(),0x20080,f.bytes.get()+128,4,f.bytes.get()+2*page_size+128,4);
        return f.t.after(x,uint64_t(f.value(128))<<32|f.value(2*page_size+128));};
      observe();f.t.phase(1,true,false,true);observe();
      f.poke(2*page_size+128,9);f.t.host_written(f.bytes.get()+2*page_size+128,4,HostWriter::Memset);
      assert(observe().reason==HashReason::HostNotice);
    }
    { Fixture f; f.t.phase(0,false,true);
      auto token=f.t.before(f.ptes.get(),0x20ffc,f.bytes.get()+4092,4);f.t.after(token,0);
      token=f.t.before(f.ptes.get(),0x21000,f.bytes.get()+8192,4);f.t.after(token,0);
      f.t.phase(1,true,false,true);
      auto inspect=[&](uint32_t g,unsigned b){auto x=f.t.before(f.ptes.get(),g,f.bytes.get()+b,4);return f.t.after(x,f.value(b));};
      inspect(0x20ffc,4092);inspect(0x21000,8192);
      uint64_t value=0x1122334455667788ull;assert(f.t.write(f.ptes.get(),0x20ffc,&value,8)==Access::Complete);
      assert(inspect(0x20ffc,4092).reason==HashReason::CpuWrite && inspect(0x21000,8192).reason==HashReason::CpuWrite);
      assert(f.value(4096)==0); // Gap/sentinel page wasn't corrupted.
    }
    { Fixture f; f.arm(); // Bound a hot mixed page without silently claiming coverage.
      for(unsigned i=0;i<100000;++i) f.store(0x20000,i);
      auto s=f.t.snapshot();assert(s.phases[1].guard_budget_exhausted==1 && !s.observing);
      assert(f.writes[0x20] && f.writes[0x40]);f.poke(128,1);
      assert(f.inspect(128).reason==HashReason::Uncovered);
    }
}
