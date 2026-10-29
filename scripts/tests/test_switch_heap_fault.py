#!/usr/bin/env python3
"""Host gates for the actual scoped handler/controller and real A64 assembly.

POSIX mprotect backs the SVC fixture. Exception frames are SYNTHETIC; neither
Horizon exception delivery nor original-PC retry is claimed by this host test.
The complete emitted wrapper is separately cross-assembled, never executed here.
"""
import argparse
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
STUB = r'''
#pragma once
#include <cstdint>
#include <cstddef>
#include <chrono>
#include <sys/mman.h>
using u32=uint32_t;using Result=uint32_t;
constexpr unsigned Perm_R=1,Perm_Rw=3,MemType_Heap=5;
struct MemoryInfo {uint64_t addr=0,size=0; uint32_t type=0,attr=0,perm=0;};
struct ThreadExceptionFrameA64 {uint64_t cpu_gprs[9],lr,sp,elr_el1;uint32_t pstate,afsr0,afsr1,esr;uint64_t far;};
#define R_FAILED(x) ((x)!=0)
#define R_SUCCEEDED(x) ((x)==0)
inline thread_local int fixture_tls;
inline void*armGetTls(){return &fixture_tls;}
inline uint64_t armGetSystemTick(){return std::chrono::steady_clock::now().time_since_epoch().count();}
inline bool envIsSyscallHinted(unsigned){return true;}
inline uintptr_t fixture_base=0;inline uint32_t fixture_perm[16]{};
inline bool fixture_ro_failure=false,fixture_rw_failure=false,fixture_query_failure=false;
inline uint32_t fixture_attr=0;inline unsigned permission_calls=0;
inline Result svcQueryMemory(MemoryInfo*out,u32*pi,uint64_t addr){
 if(fixture_query_failure||addr<fixture_base||addr>=fixture_base+16*4096)return 1;
 *pi=0;*out={addr&~uint64_t(4095),4096,MemType_Heap,fixture_attr,fixture_perm[(addr-fixture_base)/4096]};return 0;
}
inline Result svcSetMemoryPermission(void*p,size_t n,unsigned perm){
 ++permission_calls;if((perm==Perm_R&&fixture_ro_failure)||(perm==Perm_Rw&&fixture_rw_failure))return 0xd401;
 if(mprotect(p,n,perm==Perm_R?PROT_READ:PROT_READ|PROT_WRITE))return 2;
 for(size_t o=0;o<n;o+=4096)fixture_perm[(reinterpret_cast<uintptr_t>(p)+o-fixture_base)/4096]=perm;return 0;
}
'''
SOURCE = r'''
#include <util/switch_heap_fault_probe.h>
#include <util/heap_fault_policy.h>
#include <cassert>
#include <thread>
#include <filesystem>
#include <string>
#include <fstream>
namespace switch_validation {
bool heap_fault_probe_enabled(){return true;}
void record(const char*,const char*,const char*,const char*,uint64_t,uint64_t) noexcept {}
}
// Unused native helper stubs resolve the platform suite object only. No host
// result from these functions is reported as a native register/retry test.
extern "C" {
void v3k_heap_store8(void*,uint64_t){} void v3k_heap_store16(void*,uint64_t){}
void v3k_heap_store32(void*,uint64_t){} void v3k_heap_store64(void*,uint64_t){}
void v3k_heap_store128(void*,uint64_t){} void v3k_heap_register_test(void*,void*){}
char v3k_heap_store8_pc[1],v3k_heap_store16_pc[1],v3k_heap_store32_pc[1],v3k_heap_store64_pc[1],v3k_heap_store128_pc[1],v3k_heap_register_test_pc[1];
}
#include "PLATFORM_SOURCE"
int main(int argc,char**argv){
 assert(argc==2);namespace sv=switch_validation;namespace f=sv::fault;
 const uint32_t esr=(0x24u<<26)|(1u<<6)|0xf;
 f::Evidence e{0x1000,0x9000,esr,0};
 assert(f::admissible(e,0x1000,4096,0x9000,1,true));
 assert(!f::admissible(e,0x1000,4096,0x9000,1,false));
 assert(!f::admissible(e,0x1000,4096,0x9000,0,true));
 assert(!f::admissible(e,0x1000,4096,0x9004,1,true));
 for(auto v:{0u,(0x20u<<26)|0xf,esr&~64u,esr|1024u,(esr&~63u)|7u}){auto b=e;b.esr=v;assert(!f::admissible(b,0x1000,4096,0,1,true));}
 auto b=e;b.pstate=0x10;assert(!f::admissible(b,0x1000,4096,0,1,true));
 b=e;b.far=0x2000;assert(!f::admissible(b,0x1000,4096,0,1,true));
 assert(!f::admissible(e,0x1001,4096,0,1,true));
 assert(!f::admissible(e,0x1000,0,0,1,true));
 assert(!f::admissible(e,uintptr_t(-4096),8192,0,1,true));
 auto*p=static_cast<uint8_t*>(memalign(4096,16*4096));assert(p);
 fixture_base=reinterpret_cast<uintptr_t>(p);std::fill_n(fixture_perm,16,Perm_Rw);
 sv::checkpoint_ok=true;
 assert(sv::v3k_try_heap_fault(0,nullptr)==0);
 assert(!sv::heap_fault_arm(p+1,4096));
 assert(sv::heap_fault_arm(p,8192,0x9000));assert(!sv::heap_fault_arm(p,4096));
 ThreadExceptionFrameA64 frame{};frame.far=fixture_base;frame.elr_el1=0x9000;frame.esr=esr;
 auto prior=frame;unsigned calls=permission_calls;
 std::thread wrong_owner([&]{assert(sv::v3k_try_heap_fault(0,&frame)==0);});wrong_owner.join();
 assert(permission_calls==calls);
 frame.esr&=~64u;assert(sv::v3k_try_heap_fault(0,&frame)==0);frame=prior;
 assert(sv::v3k_try_heap_fault(0,&frame)==1);
 assert(std::memcmp(&frame,&prior,sizeof(frame))==0);p[0]=0x39;
 assert(sv::v3k_try_heap_fault(0,&frame)==0); // Already opened page is not re-admitted.
 frame.far+=4096;assert(sv::v3k_try_heap_fault(0,&frame)==1);p[4096]=0x45;
 auto out=sv::heap_fault_disarm();assert(out.restored&&out.faults==2&&out.dirty_mask==3);
 fixture_ro_failure=true;assert(!sv::heap_fault_arm(p,4096));assert(sv::heap_fault_safe_to_chainload());
 fixture_attr=1;auto pinned=sv::heap_permission_roundtrip(p,4096);
 assert(!pinned.read_only&&pinned.writable_after&&pinned.read_rc==0xd401);
 fixture_attr=0;fixture_ro_failure=false;
 assert(sv::heap_fault_arm(p,4096));fixture_rw_failure=true;frame.far=fixture_base;
 assert(sv::v3k_try_heap_fault(0,&frame)==0);out=sv::heap_fault_disarm();assert(!out.restored&&!sv::heap_fault_safe_to_chainload());
 fixture_rw_failure=false;assert(!svcSetMemoryPermission(p,4096,Perm_Rw));std::free(p);
 // Actual checkpoint writes/fsync and export. Synthetic frame tests above never
 // invoke the native startup suite, so no deliberate host fault is hidden here.
 std::filesystem::current_path(argv[1]);std::filesystem::create_directory("session");
 std::snprintf(sv::checkpoint_path,sizeof(sv::checkpoint_path),"checkpoint.json");
 assert(sv::heap_fault_checkpoint("synthetic_test",true));
 std::ifstream in("checkpoint.json");std::string text((std::istreambuf_iterator<char>(in)),{});assert(text.find("armed")!=std::string::npos);
 assert(sv::heap_fault_checkpoint("synthetic_test",false));sv::copy_heap_fault_checkpoint("session");
 assert(std::filesystem::exists("session/heap-fault-checkpoint.json"));
 std::snprintf(sv::checkpoint_path,sizeof(sv::checkpoint_path),"missing/path.json");
 assert(!sv::heap_fault_checkpoint("no_marker",true));assert(!sv::checkpoint_ok);
}
'''
CONTROLLER = r'''
#include <util/switch_heap_fault_probe.h>
#include <util/switch_memory_validation.h>
#include <util/switch_content_compare.h>
#include <util/switch_write_watch_api.h>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <cstring>
namespace sv=switch_validation;
int main(int argc,char**argv){
 assert(argc==2);std::filesystem::current_path(argv[1]);std::filesystem::create_directories("sdmc:/switch/vita3k");
 std::ofstream("sdmc:/switch/vita3k/tracelab.cfg")<<"mode=auto\nmemory_validation=auto\nvalidation_experiment=content_compare\n";
 sv::begin_session("HEAP");assert(sv::heap_fault_probe_enabled());assert(!sv::content_compare_enabled());
 assert(!sv::strict_hash()&&!sv::live_write_table(nullptr));sv::request_benchmark();assert(!sv::status_snapshot().restart_pending);
 sv::record("heap_fault","native_suite_complete","pass","controller fixture, not native execution");
 sv::record("heap_fault","jit_suite_complete","pass","controller fixture, not native execution");
 sv::record("heap_import","while_imported","skip","RO denied; RW retained");
 sv::record("heap_import","suite_complete","pass","fixture");
 sv::on_render_end(10000);auto s=sv::status_snapshot();
 assert(s.heap_fault&&s.controls_complete&&s.stage==sv::Stage::Complete&&s.total_ms==0);
 assert(s.heap_import_done&&!s.heap_import_ro);auto hud=sv::make_hud_text(s,10000);
 assert(std::strstr(hud.title.data(),"DONE - REPORT READY")&&std::strstr(hud.action.data(),"SAVE"));
 sv::frontend_poll(true);assert(sv::status_snapshot().stage==sv::Stage::Complete);
 std::filesystem::create_directory("report");sv::write_report("report");sv::end_session();
 sv::begin_session("FAIL");sv::record("heap_fault","previous_abort","fail","blocked");sv::on_render_end(20000);
 s=sv::status_snapshot();assert(!s.controls_complete&&std::strstr(sv::make_hud_text(s,20000).title.data(),"CHECK REPORT"));sv::end_session();
 std::ofstream("sdmc:/switch/vita3k/tracelab.cfg")<<"mode=off\nvalidation_experiment=heap_fault\n";
 sv::begin_session("OFF");assert(!sv::heap_fault_probe_enabled());sv::end_session();
 std::filesystem::remove("sdmc:/switch/vita3k/tracelab.cfg");
 sv::begin_session("NO_CONFIG");assert(sv::heap_fault_probe_enabled()&&!sv::content_compare_enabled()&&!sv::strict_hash());
 sv::request_benchmark();assert(!sv::status_snapshot().restart_pending);sv::on_render_end(30000);
 assert(sv::status_snapshot().stage==sv::Stage::Complete);sv::end_session();
}
'''

def assembly_gate(tmp):
    source=(ROOT/'vita3k/util/src/switch_heap_fault_entry.cpp').read_text()
    asm=source.split('R"A64(')[1].split(')A64"')[0]
    # No large allocation on the 0x148-byte initial PLR stack.
    assert asm.index('mov sp, x2') < asm.index('sub sp, sp, #800')
    assert asm.count('str x8, [sp, #792]')==1
    wrapper=asm.split('.size __wrap___libnx_exception_entry')[0]
    for i in range(0,32,2):
        assert wrapper.count(f'stp q{i}, q{i+1},')==1
        assert wrapper.count(f'ldp q{i}, q{i+1},')==2
    for special in ('fpcr','fpsr','nzcv'):
        assert wrapper.count('msr '+special+',')==2
    assert 'svc #0x28' in wrapper and 'b __real___libnx_exception_entry' in wrapper
    assert 'str ' not in wrapper.split('bl v3k_try_heap_fault')[1] # restore paths don't write frame
    path=tmp/'entry.s';path.write_text(asm)
    cross=shutil.which('aarch64-none-elf-g++')
    clang=shutil.which('clang++')
    if not cross and not clang: raise RuntimeError('Need cross compiler or clang for mandatory A64 assembly gate')
    command=[cross] if cross else [clang,'--target=aarch64-none-elf']
    subprocess.run(command+['-c',str(path),'-o',str(tmp/'entry.o')],check=True)
    # Link a real A64 call-site fixture to prove --wrap redirection and fallback.
    (tmp/'caller.s').write_text('.text\n.global _start\n_start:\nb __libnx_exception_entry\n')
    (tmp/'stock.s').write_text('.text\n.weak __libnx_exception_entry\n__libnx_exception_entry:\nret\n.global v3k_try_heap_fault\nv3k_try_heap_fault:\nmov w0,wzr\nret\n')
    subprocess.run(command+['-nostdlib','-Wl,--wrap=__libnx_exception_entry','-Wl,-e,_start',str(tmp/'caller.s'),str(tmp/'stock.s'),str(tmp/'entry.o'),'-o',str(tmp/'linked.elf')],check=True)
    dump=shutil.which('aarch64-none-elf-objdump') or shutil.which('llvm-objdump')
    if not dump: raise RuntimeError('Need A64 objdump to verify wrap target')
    text=subprocess.check_output([dump,'-d',str(tmp/'linked.elf')],text=True)
    start=text.split('<_start>:')[1].split('\n\n')[0]
    assert '__wrap___libnx_exception_entry' in start
    assert '<__libnx_exception_entry>' in text


def main():
    p=argparse.ArgumentParser();p.add_argument('--cxx',default='c++');p.add_argument('--sanitize',action='store_true');a=p.parse_args()
    with tempfile.TemporaryDirectory(prefix='vita3k-heap-fault-') as name:
        t=Path(name);inc=t/'include';(inc/'util').mkdir(parents=True);(inc/'switch/arm').mkdir(parents=True)
        (inc/'switch.h').write_text(STUB)
        (inc/'util/log.h').write_text('#pragma once\n#define LOG_INFO(...) ((void)0)\n#define LOG_WARN(...) ((void)0)\n#define LOG_ERROR(...) ((void)0)\n')
        (inc/'switch/arm/counter.h').write_text('#pragma once\n#include <cstdint>\ninline uint64_t armGetSystemTickFreq(){return 1000;}\n')
        flags=[a.cxx,'-std=c++20','-O2','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-pthread','-D__SWITCH__','-DVITA3K_SWITCH_TRACELAB=1','-I',str(inc),'-I',str(ROOT/'vita3k/util/include')]
        if a.sanitize:flags+=['-O1','-fsanitize=address,undefined','-fno-omit-frame-pointer']
        (t/'platform.cpp').write_text(SOURCE.replace('PLATFORM_SOURCE',str(ROOT/'vita3k/util/src/switch_heap_fault_probe.cpp')))
        subprocess.run(flags+[str(t/'platform.cpp'),'-o',str(t/'platform')],check=True)
        subprocess.run([str(t/'platform'),str(t)],check=True,timeout=20)
        (t/'controller.cpp').write_text(CONTROLLER)
        subprocess.run(flags+['-DVITA3K_SWITCH_HEAP_FAULT_DEFAULT=1','-DVITA3K_SWITCH_CLEAN_CAPTURE_DEFAULT=1','-DVITA3K_SWITCH_WRITE_SHADOW_DEFAULT=1',str(t/'controller.cpp'),str(ROOT/'vita3k/util/src/switch_memory_validation.cpp'),'-o',str(t/'controller')],check=True)
        subprocess.run([str(t/'controller'),str(t)],check=True,timeout=20)
        report=json.loads((t/'report/memory-validation.json').read_text());assert report['capture_plan']=='heap-fault-1' and not report['gameplay_benchmark']
        spec=importlib.util.spec_from_file_location('capture_analyzer',ROOT/'scripts/analyze-switch-capture.py')
        module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
        assert module.analyze(report)['CPU_recovery_controls_passed'] is False # Only two summary fixtures, no native records.
        required=['native_store_8','native_store_16','native_store_32','native_store_64','native_store_128',
                  'register_context','cross_page_128','host_memset','rearm_64','native_cleanup',
                  'native_suite_complete','jit_direct_8','jit_direct_16','jit_direct_32','jit_direct_64',
                  'jit_software_alias','jit_suite_complete']
        report['tests']=[dict(suite='heap_fault',name=n,status='pass',detail='analyzer fixture',elapsed_ticks=0,iterations=0) for n in required]
        result=module.analyze(report);assert result['CPU_recovery_controls_passed'] and not result['comparison_eligible']
        assert result['GPU_import_reprotect']=='not_exercised'
        report['tests'].append(dict(suite='heap_import',name='while_imported',status='skip'))
        assert module.analyze(report)['GPU_import_reprotect']=='RO_not_supported'
        report['tests'][0]['status']='fail';assert not module.analyze(report)['CPU_recovery_controls_passed']
        assembly_gate(t)
    print('Heap fault: actual admission/handler/cleanup with mprotect + synthetic frames, actual controller/HUD/export and real A64 assembly/wrap link passed; no Horizon/native execution claim')

if __name__=='__main__':main()
