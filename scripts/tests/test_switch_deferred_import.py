#!/usr/bin/env python3
"""Real deferred registry + renderer wrapper/gateways with an explicit fake Vulkan materializer.
Full CI extracts the actual expanded gateways. The sparse local fallback is a
literal of the reviewed baseline gateway, transformed by the exact new hooks.
No host result is presented as Vulkan/Horizon or rendered-output evidence.
"""
import argparse
import json
import subprocess
import tempfile
from pathlib import Path
ROOT = Path(__file__).resolve().parents[2]
ORIGINAL_GATEWAYS = r'''
std::tuple<vk::Buffer, uint32_t> VKState::get_matching_mapping(const Ptr<void> address) {
    // A game can map a range inside one it already mapped. The nearest base below an address can
    // be such an inner range and need not reach the address, so walk down to the one enclosing it.
    for (auto mapped_memory = mapped_memories.lower_bound(address.address());
         mapped_memory != mapped_memories.end(); ++mapped_memory) {
        if (mapped_memory->first + mapped_memory->second.size > address.address())
            return std::make_tuple(mapped_memory->second.buffer,
                mapped_memory->second.buffer_offset + address.address() - mapped_memory->first);
    }

    // logging an error flushes the log file, which is far too slow to do once per draw
    LOG_ERROR_ONCE("Could not find matching mapped buffer for guest address {:#x}", address.address());
    return { nullptr, 0 };
}

std::tuple<uint64_t, int32_t, int32_t> VKState::get_matching_device_address(const Address address) {
    // an inner mapping must not hide the one that encloses the address, as above
    auto mapped_memory = mapped_memories.lower_bound(address);
    while (mapped_memory != mapped_memories.end()
        && static_cast<uint64_t>(mapped_memory->first) + mapped_memory->second.size <= address)
        ++mapped_memory;
    if (mapped_memory == mapped_memories.end()) {
        LOG_ERROR_ONCE("Could not find matching mapped buffer for guest address {:#x}", address);
        return { 0, 0, 0 };
    }

    const int64_t offset = address - mapped_memory->first;
    const int64_t lower_bound = -offset;
    const int64_t upper_bound = static_cast<int64_t>(mapped_memory->second.size) - offset;
    constexpr int64_t min_bound = std::numeric_limits<int32_t>::min();
    constexpr int64_t max_bound = std::numeric_limits<int32_t>::max();

    return {
        mapped_memory->second.buffer_address + offset,
        static_cast<int32_t>(std::clamp(lower_bound, min_bound, max_bound)),
        static_cast<int32_t>(std::clamp(upper_bound, min_bound, max_bound))
    };
}
'''
STUB = r'''
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstddef>
#include <map>
#include <limits>
#include <tuple>
#include <variant>
#include <stdexcept>
using Address=uint32_t;
namespace vk { struct Buffer { uint64_t value=0; Buffer()=default; Buffer(std::nullptr_t){}; Buffer(uint64_t v):value(v){} explicit operator bool()const{return value!=0;} }; }
struct MemState { uintptr_t host=0; uint32_t base=4096,size=65536; bool valid=true,flat=true; };
template<class T> struct Ptr { uint32_t a; explicit Ptr(uint32_t v):a(v){} uint32_t address()const{return a;}
 T* get(const MemState&m)const{return reinterpret_cast<T*>(m.host+a-m.base);} };
inline bool is_valid_addr_range_size(const MemState&m,uint32_t a,uint32_t s){return m.valid&&a>=m.base&&uint64_t(a)+s<=uint64_t(m.base)+m.size;}
enum class MappingMethod { ExernalHost, DoubleBuffer };
namespace renderer::vulkan {
struct ExternalBuffer { vk::Buffer memory;void*extra;};
struct FakeGpuBuffer {};
struct MappedMemory {Address address;std::variant<ExternalBuffer,FakeGpuBuffer> buffer_impl;vk::Buffer buffer;uint32_t size;uint64_t buffer_address;
 uint32_t buffer_offset=0;uint64_t switch_lazy_token=0;uint32_t switch_lazy_uses=0;};
struct VKState {MemState*mem;MappingMethod mapping_method=MappingMethod::ExernalHost;
 std::map<Address,MappedMemory,std::greater<Address>> mapped_memories;
 unsigned creates=0;bool fallback=false,fail=false,zero=false;
 bool map_memory(MemState&,Ptr<void>,uint32_t);bool map_memory_eager(MemState&,Ptr<void>,uint32_t);
 bool switch_ensure_mapping(Address,uint32_t);
 std::tuple<vk::Buffer,uint32_t> get_matching_mapping(Ptr<void>);
 std::tuple<uint64_t,int32_t,int32_t> get_matching_device_address(Address);
};
}
'''
HARNESS = r'''
#include <renderer/vulkan/switch_deferred_import_impl.h>
#include <cassert>
#include <vector>
#include <thread>
namespace renderer::vulkan {
static bool switch_range_alias_is_flat(const MemState&m,Address,uint32_t){return m.flat;}
bool VKState::map_memory_eager(MemState&m,Ptr<void>a,uint32_t n){
 ++creates;if(fail)throw std::runtime_error("fake driver allocation failure");
 mapped_memories[a.address()]={a.address(),ExternalBuffer{vk::Buffer(1),a.get(m)},vk::Buffer(1),n,zero?0:0x100000000ull};
 if(fallback)mapped_memories.at(a.address()).buffer_impl=FakeGpuBuffer{};
 return true;
}
GATEWAYS
}
int main(int argc,char**argv){
 assert(argc==2);namespace li=switch_validation::deferred_import;namespace dh=switch_validation::direct_heap;
 auto&r=li::registry;assert(r.configure(true));dh::state.active.store(true);
 MemState mem{0x100000,4096,65536,true,true};renderer::vulkan::VKState vk{&mem};
 const auto owner=reinterpret_cast<uintptr_t>(&vk);
 assert(vk.map_memory(mem,Ptr<void>(4096),32768));assert(vk.creates==0);
 auto token=vk.mapped_memories.at(4096).switch_lazy_token;assert(token);
 assert(r.blocks_recycle(mem.host,4096));assert(!r.blocks_recycle(mem.host+65536,4096));
 assert(vk.map_memory(mem,Ptr<void>(4096),32768));assert(vk.creates==0&&r.summary().registered==1);
 auto [buffer,offset]=vk.get_matching_mapping(Ptr<void>(4112));assert(buffer&&offset==16&&vk.creates==1);
 for(int i=0;i<10000;++i){auto[x,y]=vk.get_matching_mapping(Ptr<void>(4112));assert(x&&y==16);}
 assert(vk.creates==1);auto [address,lo,hi]=vk.get_matching_device_address(4112);
 assert(address==0x100000010ull&&lo==-16&&hi==32752&&vk.creates==1);
 assert(r.get(token).uses==(li::Buffer|li::DeviceAddress));
 assert(vk.map_memory(mem,Ptr<void>(8192),4096)); // Nested mapping must not hide outer ranges.
 auto [outer,o]=vk.get_matching_mapping(Ptr<void>(16384));assert(outer&&o==12288&&vk.creates==1);
 assert(vk.switch_ensure_mapping(8192,li::CpuSync|li::Writable|li::Uniform));assert(vk.creates==2);
 auto inner=vk.mapped_memories.at(8192).switch_lazy_token;
 r.release(token);vk.mapped_memories.erase(4096);assert(r.blocks_recycle(mem.host+4096,4096));
 r.release(inner);vk.mapped_memories.erase(8192);assert(!r.blocks_recycle(mem.host,32768));
 assert(vk.map_memory(mem,Ptr<void>(4096),4096));auto second=vk.mapped_memories.at(4096).switch_lazy_token;assert(second!=token);
 r.freeze_window();r.release(second);vk.mapped_memories.clear();
 assert(r.summary().pending==1&&r.summary().frozen);r.start_window();assert(r.summary().avoided==1);
 // Error and stale backing never become a published fake device address.
 assert(vk.map_memory(mem,Ptr<void>(4096),4096));auto stale=vk.mapped_memories.at(4096).switch_lazy_token;
 mem.host+=0x10000;bool threw=false;try{vk.get_matching_device_address(4096);}catch(...){threw=true;}
 assert(threw&&r.get(stale).phase==li::Phase::Failed&&r.blocks_recycle(mem.host-0x10000,4096));
 r.release_owner(owner);vk.mapped_memories.clear();assert(r.configure(true));
 vk.fallback=true;assert(vk.map_memory(mem,Ptr<void>(4096),4096));vk.get_matching_mapping(Ptr<void>(4096));assert(r.summary().fallbacks==1);
 r.release_owner(owner);vk.mapped_memories.clear();assert(r.configure(true));vk.fallback=false;
 for(bool zero:{false,true}){vk.zero=zero;vk.fail=!zero;assert(vk.map_memory(mem,Ptr<void>(4096),4096));threw=false;
 try{vk.get_matching_mapping(Ptr<void>(4096));}catch(...){threw=true;}assert(threw&&r.summary().errors>0);
 r.release_owner(owner);vk.mapped_memories.clear();assert(r.configure(true));}
 vk.zero=vk.fail=false;
 // Fixed capacity falls back eagerly rather than weakening lifetime tracking.
 for(size_t i=0;i<li::Registry::capacity;++i)assert(r.reserve(1,uint32_t(4096+i*4096),0x80000000+i*4096,4096));
 unsigned before=vk.creates;assert(vk.map_memory(mem,Ptr<void>(4096),4096));assert(vk.creates==before+1);
 assert(r.summary().capacity_fallbacks==1&&!vk.mapped_memories.at(4096).switch_lazy_token);
 r.release_owner(1);vk.mapped_memories.clear();assert(r.configure(true));
 // Registry concurrency only; this is not concurrent Vulkan or Guest testing.
 std::vector<std::thread> threads;
 for(unsigned t=0;t<4;++t)threads.emplace_back([&,t]{for(unsigned i=0;i<100;++i){auto k=r.reserve(t+1,4096*(i+1),0x10000000+(t*128+i)*4096,4096);assert(k);r.release(k);}});
 for(auto&t:threads){t.join();}assert(r.summary().avoided==400&&r.summary().errors==0);
 // The original buffer lookup used a 32-bit end and failed at the 4GiB edge.
 MemState top{0x900000,0xfffff000u,4096,true,true};renderer::vulkan::VKState edge{&top};
 assert(edge.map_memory(top,Ptr<void>(0xfffff000u),4096));
 auto[edge_buffer,edge_offset]=edge.get_matching_mapping(Ptr<void>(0xfffffffcu));
 assert(edge_buffer&&edge_offset==4092);
 auto[edge_address,edge_lo,edge_hi]=edge.get_matching_device_address(0xfffffffcu);
 assert(edge_address==0x100000ffcull&&edge_lo==-4092&&edge_hi==4);
 r.release_owner(reinterpret_cast<uintptr_t>(&edge));edge.mapped_memories.clear();
 r.freeze_window();FILE*f=std::fopen(argv[1],"w");assert(f);r.write_json(f);std::fclose(f);
 assert(r.configure(false));before=vk.creates;assert(vk.map_memory(mem,Ptr<void>(4096),4096));assert(vk.creates==before+1);
}
'''
def main():
    p=argparse.ArgumentParser();p.add_argument('--cxx',default='c++');p.add_argument('--sanitize',action='store_true');p.add_argument('--require-source',action='store_true');a=p.parse_args()
    path=ROOT/'vita3k/renderer/src/vulkan/renderer.cpp'
    if path.is_file():
        text=path.read_text();begin=text.index('std::tuple<vk::Buffer, uint32_t> VKState::get_matching_mapping(')
        gateways=text[begin:text.index('int VKState::get_max_anisotropic_filtering()',begin)]
        assert 'switch_ensure_mapping' in gateways and 'bool VKState::map_memory_eager(' in text
        assert 'state.switch_ensure_mapping(mem_it->first' in text
        assert 'registry.release(ite->second.switch_lazy_token)' in text
        assert 'registry.release_owner(reinterpret_cast<uintptr_t>(this))' in text
        assert '#include <renderer/vulkan/switch_deferred_import_impl.h>' in text
        print('Testing actual expanded renderer gateways')
    else:
        assert not a.require_source,'Complete checkout required for production gateway gate'
        gateways=ORIGINAL_GATEWAYS
        recipe=json.loads((ROOT/'patches/experiments/switch-deferred-import.json').read_text())
        for item in recipe['files']:
            if item['path']=='vita3k/renderer/src/vulkan/renderer.cpp':
                for edit in item['replacements']:
                    if edit['old'] in gateways:assert gateways.count(edit['old'])==1;gateways=gateways.replace(edit['old'],edit['new'])
        assert gateways.count('switch_ensure_mapping')==2
        print('Sparse local checkout: reviewed gateway literal transformed with real hooks, not complete-source verification')
    with tempfile.TemporaryDirectory() as directory:
        d=Path(directory)
        for n in ['renderer/vulkan','mem','util','switch/arm']:(d/n).mkdir(parents=True,exist_ok=True)
        (d/'renderer/vulkan/state.h').write_text(STUB)
        (d/'mem/functions.h').write_text('#pragma once\n')
        (d/'util/log.h').write_text('#pragma once\n#define LOG_ERROR(...) ((void)0)\n#define LOG_ERROR_ONCE(...) ((void)0)\n')
        (d/'switch/arm/counter.h').write_text('#pragma once\n#include <cstdint>\ninline uint64_t armGetSystemTick(){static uint64_t tick=0;return ++tick;}\n')
        (d/'test.cpp').write_text(HARNESS.replace('GATEWAYS',gateways))
        cmd=[a.cxx,'-std=c++20','-O2','-Wall','-Wextra','-Werror','-Wno-missing-field-initializers','-D__SWITCH__','-DVITA3K_SWITCH_TRACELAB=1','-pthread',
             '-I',str(d),'-I',str(ROOT/'vita3k/util/include'),'-I',str(ROOT/'vita3k/renderer/include')]
        if a.sanitize:cmd+=['-fsanitize=address,undefined','-fno-omit-frame-pointer']
        subprocess.run(cmd+[str(d/'test.cpp'),'-o',str(d/'test')],check=True)
        subprocess.run([str(d/'test'),str(d/'report.json')],check=True,timeout=20)
        report=json.loads((d/'report.json').read_text());assert report['registered']==401 and report['released_without_import']==400 and not report['errors']
    print('Deferred registry, real renderer wrapper/gateways, lifetime leases, aliases, fallback, failures, capacity and frozen report passed; fake Vulkan materializer, NOT device execution')
if __name__=='__main__':main()
