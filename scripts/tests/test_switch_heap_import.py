#!/usr/bin/env python3
"""Compile the real private-import probe against an explicit non-GPU API fixture.
Tests allocation/permission/failure control flow, NOT Vulkan ABI or driver support.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[2]
SOURCE=r'''
#include <cstdint>
#include <string>
#include <stdexcept>
#include <vector>
#include <cassert>
#include <util/switch_heap_fault_probe.h>
struct Row{std::string name,status;};std::vector<Row> rows;
int scenario=0;bool pinned=false,retained=false;int allocated=0,destroyed=0,released=0,marks=0;
void*host_pointer=nullptr;
namespace vk {
enum class MemoryPropertyFlagBits:unsigned{eHostCached=8};inline unsigned operator&(unsigned a,MemoryPropertyFlagBits b){return a&unsigned(b);}
enum class ExternalMemoryHandleTypeFlagBits{eHostAllocationEXT};enum class MemoryAllocateFlagBits{eDeviceAddress};enum class SharingMode{eExclusive};
using BufferUsageFlags=unsigned;
struct Buffer{int n=0;explicit operator bool()const{return n;}};struct DeviceMemory{int n=0;explicit operator bool()const{return n;}};
struct PhysicalDeviceExternalMemoryHostPropertiesEXT{uint64_t minImportedHostPointerAlignment=4096;};
struct PhysicalDeviceProperties2{void*pNext=nullptr;};
struct PhysicalDeviceMemoryProperties{uint32_t memoryTypeCount=1;struct T{unsigned propertyFlags=8;}memoryTypes[32];};
struct BufferDeviceAddressInfoKHR{Buffer buffer;};
struct BufferCreateInfo{uint64_t size;unsigned usage;SharingMode sharingMode;};
struct ExternalMemoryBufferCreateInfoKHR{ExternalMemoryHandleTypeFlagBits handleTypes;};
struct MemoryAllocateInfo{uint64_t allocationSize;uint32_t memoryTypeIndex;};
struct ImportMemoryHostPointerInfoEXT{ExternalMemoryHandleTypeFlagBits handleType;void*pHostPointer;};
struct MemoryAllocateFlagsInfo{MemoryAllocateFlagBits flags;};
template<class A,class...B>struct StructureChain{A first;StructureChain(A a,B...):first(a){}const A&get()const{return first;}};
struct PhysicalDevice{void getProperties2(PhysicalDeviceProperties2*p){static_cast<PhysicalDeviceExternalMemoryHostPropertiesEXT*>(p->pNext)->minImportedHostPointerAlignment=4096;}};
struct Device{
 Buffer createBuffer(const BufferCreateInfo&){++allocated;return {1};}
 struct Req{uint64_t size=65536;uint32_t memoryTypeBits=1;};Req getBufferMemoryRequirements(Buffer){return {};}
 struct Props{uint32_t memoryTypeBits=1;};Props getMemoryHostPointerPropertiesEXT(ExternalMemoryHandleTypeFlagBits,void*p){host_pointer=p;return {};}
 DeviceMemory allocateMemory(const MemoryAllocateInfo&){if(scenario==2)throw std::runtime_error("expected import failure");pinned=true;return {1};}
 void bindBufferMemory(Buffer,DeviceMemory,uint64_t){}
 uint64_t getBufferAddress(const BufferDeviceAddressInfoKHR&){return 0x100000;}
 void destroyBuffer(Buffer){++destroyed;}
 void freeMemory(DeviceMemory){assert(destroyed==1);++released;pinned=false;}
};
}
namespace switch_validation{
bool heap_fault_probe_enabled(){return true;}
bool heap_fault_checkpoint(const char*,bool)noexcept{++marks;return scenario!=4;}
void record(const char*,const char*name,const char*status,const char*,uint64_t,uint64_t)noexcept{rows.push_back({name,status});}
HeapPermissionResult heap_permission_roundtrip(void*,size_t)noexcept{
 HeapPermissionResult r;r.query_before=true;r.read_only=!(scenario==1&&pinned);r.read_rc=r.read_only?0:0xd401;
 r.writable_after=!(scenario==3&&released);return r;
}
void heap_fault_retain_allocation()noexcept{retained=true;}
}
#include <renderer/vulkan/switch_heap_import_probe.h>
int main(int argc,char**argv){
 assert(argc==2);scenario=std::stoi(argv[1]);vk::PhysicalDeviceMemoryProperties p;
 switch_validation::probe_gpu_heap_import({}, {},p,0);switch_validation::probe_gpu_heap_import({}, {},p,0);
 auto status=[&](const char*name){for(auto&r:rows)if(r.name==name)return r.status;return std::string("missing");};
 if(scenario==4){assert(allocated==0&&marks==1&&status("checkpoint_unavailable")=="skip");return 0;}
 assert(allocated==1&&destroyed==1&&marks==2&&!pinned);
 assert(status("before_import")=="pass");
 if(scenario==0)assert(status("while_imported")=="pass"&&status("suite_complete")=="pass"&&!retained);
 if(scenario==1)assert(status("while_imported")=="skip"&&status("suite_complete")=="pass"&&!retained);
 if(scenario==2)assert(status("import_unavailable")=="skip"&&status("suite_complete")=="skip"&&released==0&&!retained);
 if(scenario==3){assert(retained&&status("after_release")=="fail"&&status("suite_complete")=="fail");std::free(host_pointer);}
}
'''
def main():
 p=argparse.ArgumentParser();p.add_argument('--cxx',default='c++');a=p.parse_args()
 with tempfile.TemporaryDirectory() as d:
  t=Path(d);(t/'test.cpp').write_text(SOURCE)
  subprocess.run([a.cxx,'-std=c++20','-O2','-Wall','-Wextra','-Werror','-D__SWITCH__','-DVITA3K_SWITCH_TRACELAB=1','-I',str(ROOT/'vita3k/util/include'),'-I',str(ROOT/'vita3k/renderer/include'),str(t/'test.cpp'),'-o',str(t/'test')],check=True)
  for case in range(5):subprocess.run([str(t/'test'),str(case)],check=True,timeout=10)
 print('Actual import-probe control flow passed 5 synthetic driver cases: support, pin denial, import failure, unsafe restore/retention, checkpoint refusal; NOT Vulkan driver validation')
if __name__=='__main__':main()
