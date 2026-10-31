#!/usr/bin/env python3
"""Execute the production canary DRIVER with a deliberately non-native JIT stub.
The observer and byte mutations are real. Does NOT validate A64 emission/SDK ABI.
"""
import argparse,subprocess,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
p=argparse.ArgumentParser();p.add_argument('--cxx',default='c++');a=p.parse_args()
s=(ROOT/'vita3k/cpu/src/switch_memory_validation.cpp').read_text()
body=s[s.index('void run_observer_canaries('):s.index('void run_config_rejections(')]
head=r'''
#include <util/switch_write_watch.h>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <array>
#include <string>
#include <vector>
namespace switch_validation {
std::vector<std::string> names;
void record(const char*,const char*name,const char*status,const char*) {
 if (std::string(status)!="pass") {std::fprintf(stderr,"canary failed: %s\n",name);std::abort();}
 names.emplace_back(name);
}
}
constexpr uint32_t kPageSize=4096,kDataPages=4,kDataBase=0x20000,kAliasBase=0x60000;
using PT=std::array<uint8_t*,1<<20>;
struct MemoryFixture;
namespace Dynarmic::A32 {
struct UserConfig{PT*write_page_table=nullptr;MemoryFixture*callbacks=nullptr;};
class Jit{public:UserConfig c;std::array<uint32_t,16> regs{};explicit Jit(UserConfig c):c(c){};
 auto&Regs(){return regs;}void Run();};
}
enum class TableMode{Mirror};
struct MemoryFixture{
 std::unique_ptr<PT>read_table{new PT{}};
 std::unique_ptr<uint8_t,decltype(&std::free)>bytes{static_cast<uint8_t*>(std::aligned_alloc(4096,32768)),&std::free};
 switch_validation::watch::Table*observer=nullptr;Dynarmic::A32::Jit*active_jit=nullptr;
 uint32_t store32=0x1000;bool stopped=false;
 MemoryFixture(){assert(bytes);for(unsigned i=0;i<4;++i)(*read_table)[kDataBase/4096+i]=reinterpret_cast<uint8_t*>(uintptr_t(backing(i))-(kDataBase+i*4096));
 (*read_table)[kAliasBase/4096]=reinterpret_cast<uint8_t*>(uintptr_t(backing(0))-kAliasBase);}
 uint8_t*backing(unsigned i){return bytes.get()+i*8192;}
 void reset_bytes(){std::memset(bytes.get(),0xa5,32768);}void reset_events(){stopped=false;}
 void prepare(Dynarmic::A32::Jit&jit,uint32_t,uint32_t addr){active_jit=&jit;jit.Regs()[0]=addr;}
 bool clean_stop(){return stopped;}
};
void Dynarmic::A32::Jit::Run(){ // Explicitly interpret ONE test STR, not native execution.
 const auto a=regs[0],v=regs[1];auto*entry=(*c.write_page_table)[a/4096];
 if(entry && (a % 4096) <= 4092)std::memcpy(reinterpret_cast<void*>(uintptr_t(entry)+a),&v,4);
 else assert(c.callbacks->observer->write(c.callbacks->read_table->data(),a,&v,4)==switch_validation::watch::Access::Complete);
 c.callbacks->stopped=true;
}
Dynarmic::A32::UserConfig config_for(MemoryFixture&m,TableMode,bool){return {nullptr,&m};}
'''
with tempfile.TemporaryDirectory() as d:
 f=Path(d)/'driver.cpp';exe=Path(d)/'driver'
 f.write_text(head+body+r'''
int main(){MemoryFixture m;run_observer_canaries(m);assert(switch_validation::names.size()==15);assert(!m.observer && !m.active_jit);}
''')
 subprocess.run([a.cxx,'-std=c++20','-O2','-Wall','-Wextra','-Werror','-pedantic','-I',str(ROOT/'vita3k/util/include'),str(f),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True,timeout=30)
print('15 production mutation-canary driver cases passed with real observer + explicitly interpreted host STR stub (NOT native A64)')
