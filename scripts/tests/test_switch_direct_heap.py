#!/usr/bin/env python3
"""Actual Direct Heap helpers and protected-cache controls.
POSIX faults exercise the production C++ handler with synthetic Horizon frames;
this is NOT execution of Horizon, A64, Vulkan, or live Guest integration.
"""
import argparse
import ctypes.util
import importlib.util
import json
import os
import shutil
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]

def load(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module); return module

POLICY = r'''
#include <util/switch_direct_heap_platform.h>
#include <util/switch_validation_status.h>
#include <cassert>
#include <filesystem>
#include <fstream>
int main(int argc,char**argv){
 assert(argc==2);namespace dh=switch_validation::direct_heap;
 std::filesystem::current_path(argv[1]);std::filesystem::create_directories("sdmc:/switch/vita3k");
 assert(dh::requested());assert(dh::lazy_import_requested());
 std::ofstream("sdmc:/switch/vita3k/tracelab.cfg")<<"gpu_import=eager\n";assert(!dh::lazy_import_requested());
 std::ofstream("sdmc:/switch/vita3k/tracelab.cfg")<<"gpu_import=deferred\n";assert(dh::lazy_import_requested());
 std::ofstream("sdmc:/switch/vita3k/tracelab.cfg")<<" guest_backing = stack_alias # comment\n";
 assert(!dh::requested());
 std::ofstream("sdmc:/switch/vita3k/tracelab.cfg")<<"guest_backing=direct_heap\nmode=off\n";
 assert(!dh::requested());
 std::ofstream("sdmc:/switch/vita3k/tracelab.cfg")<<"validation_experiment=legacy_content_compare\n";
 assert(!dh::requested());
 std::filesystem::remove("sdmc:/switch/vita3k/tracelab.cfg");
 assert(dh::budget(8ull<<30,4ull<<30)==1280ull*1024*1024);
 assert(dh::budget(4ull<<30,3ull<<30)==768ull*1024*1024);
 assert(dh::budget(4ull<<30,1536ull*1024*1024)==512ull*1024*1024);
 assert(!dh::budget(4ull<<30,1536ull*1024*1024-1));
 auto*p=static_cast<uint8_t*>(memalign(4096,16*4096));assert(p);
 fixture_base=reinterpret_cast<uintptr_t>(p);std::fill_n(fixture_perm,16,Perm_Rw);
 assert(!dh::writable_private(p+1,4096));assert(!dh::writable_private(p,4095));
 assert(dh::activate(p,16*4096));assert(dh::commit(p,p,8192)==0);
 auto& registry=switch_validation::deferred_import::registry;
 const auto lease=registry.reserve(1,4096,reinterpret_cast<uintptr_t>(p),4096);assert(lease);
 assert(dh::recycle(p,p,4096)!=0);registry.release(lease);assert(dh::recycle(p,p,4096)==0);
 assert(dh::commit(p,p+4096,4096)!=0);assert(dh::recycle(p,p,8192)==0);
 dh::request_capture();dh::frame(1,1000);
 fixture_attr=4;assert(dh::recycle(p,p,4096)!=0);assert(!dh::writable_private(p,4096));
 dh::observe(p,1024);assert(dh::snapshot().pinned_bytes==1024);
 fixture_attr=0;dh::observe(p+4096,2048);assert(dh::snapshot().plain_bytes==2048);
 dh::observe(p+4096,2048);assert(dh::snapshot().ranges==2);
 fixture_query_failure=true;dh::observe(p+8192,4096);fixture_query_failure=false;
 assert(dh::snapshot().other_bytes==4096);
 dh::state.errors.store(0);dh::frame(100,1000);dh::frame(30100,1000);
 assert(dh::state.final_classification_complete.load());assert(registry.summary().frozen);
 unsigned checked=0;for(const auto& key:dh::state.keys)if(key.begin){++checked;
   assert(key.final_plain==key.size&&key.final_pinned==0&&key.final_other==0);}
 assert(checked==3);const auto old_ranges=dh::state.ranges.load();dh::observe(p+12288,4096);
 assert(dh::state.ranges.load()==old_ranges);
 assert(dh::snapshot().finished&&!dh::ready(dh::snapshot())); // Too few rendered frames.
 dh::begin_session();assert(!dh::snapshot().finished&&dh::snapshot().ranges==0);
 dh::frame(1,1000);assert(!dh::snapshot().started);dh::request_capture();
 for(unsigned i=0;i<121;++i)dh::frame(100+i*250,1000);
 dh::state.ranges.store(1);assert(dh::ready(dh::snapshot()));
 dh::state.errors.store(1);assert(!dh::ready(dh::snapshot()));
 switch_validation::StatusSnapshot s;s.heap_fault=s.direct_guest=true;s.stage=switch_validation::Stage::Measuring;
 s.remaining_ms=1000;s.direct_pool_mib=1280;
 auto text=switch_validation::make_hud_text(s,0);assert(std::strstr(text.title.data(),"GAME VALIDATION")&&!std::strstr(text.title.data(),"DONE"));
 s.lazy_imports=true;s.lazy_pending_mib=8;s.lazy_imported_mib=4;s.lazy_avoided=3;
 text=switch_validation::make_hud_text(s,0);assert(std::strstr(text.samples.data(),"Deferred:8 MiB"));
 s.stage=switch_validation::Stage::Complete;s.outcome=switch_validation::CaptureOutcome::Ready;s.controls_complete=true;
 text=switch_validation::make_hud_text(s,0);assert(std::strstr(text.title.data(),"DONE - REPORT READY"));
 s.failed=1;text=switch_validation::make_hud_text(s,0);assert(std::strstr(text.title.data(),"CHECK REPORT"));
 FILE*f=std::fopen("topology.json","w");assert(f);dh::write_json(f);assert(!std::fclose(f));std::free(p);
}
'''
CACHE = r'''
#include <switch.h>
#include <malloc.h>
#include <cassert>
#include <csignal>
#include <filesystem>
#include <string>
#include <vector>
struct Row{std::string suite,name,status;};std::vector<Row>rows;
static bool negative=false;
static void*fixture_memalign(size_t a,size_t n){void*p=nullptr;if(posix_memalign(&p,a,n))return nullptr;
 fixture_base=reinterpret_cast<uintptr_t>(p);std::fill_n(fixture_perm,16,Perm_Rw);return p;}
#define memalign fixture_memalign
namespace switch_validation{
bool heap_fault_probe_enabled(){return true;}
void record(const char*s,const char*n,const char*t,const char*,uint64_t,uint64_t)noexcept{rows.push_back({s,n,t});}
}
extern "C" {
void v3k_heap_store8(void*p,uint64_t v){*static_cast<volatile uint8_t*>(p)=v;}
void v3k_heap_store16(void*p,uint64_t v){*static_cast<volatile uint16_t*>(p)=v;}
void v3k_heap_store32(void*p,uint64_t v){*static_cast<volatile uint32_t*>(p)=v;}
void v3k_heap_store64(void*p,uint64_t v){*static_cast<volatile uint64_t*>(p)=v;}
void v3k_heap_store128(void*p,uint64_t v){auto*d=static_cast<volatile uint64_t*>(p);d[0]=v;d[1]=v;}
void v3k_heap_register_test(void*,void*){}
char v3k_heap_store8_pc[1],v3k_heap_store16_pc[1],v3k_heap_store32_pc[1],v3k_heap_store64_pc[1],v3k_heap_store128_pc[1],v3k_heap_register_test_pc[1];
#ifndef USE_PINNED_XXHASH
uint64_t XXH3_64bits(const void*,size_t);
#endif
}
#ifdef USE_PINNED_XXHASH
#define XXH_INLINE_ALL
#include <xxhash.h>
#endif
uint64_t test_hash(const void*p,size_t n){return XXH3_64bits(p,n);}
#include "PLATFORM_SOURCE"
#include <util/switch_protected_cache_probe.h>
static void fault(int,siginfo_t*info,void*){
 ThreadExceptionFrameA64 f{};f.far=reinterpret_cast<uintptr_t>(info->si_addr);
 f.elr_el1=0x9000;f.esr=(0x24u<<26)|(1u<<6)|0xf;
 if(switch_validation::v3k_try_heap_fault(0,&f)!=1)_Exit(90);
 if(negative)switch_validation::own_slot()->result.dirty_mask=0; // Deliberately broken evidence.
}
int main(int argc,char**argv){
 assert(argc==3);negative=std::string(argv[2])=="negative";
 std::filesystem::current_path(argv[1]);namespace sv=switch_validation;
 sv::checkpoint_ok=sv::native_ok=true;std::snprintf(sv::checkpoint_path,sizeof(sv::checkpoint_path),"cache-checkpoint.json");
 sv::direct_heap::state.active.store(true);
 struct sigaction action{};action.sa_sigaction=fault;action.sa_flags=SA_SIGINFO;sigemptyset(&action.sa_mask);
 assert(sigaction(SIGSEGV,&action,nullptr)==0);
 sv::run_protected_cache_probe(&test_hash);
 assert(sv::direct_heap::state.cache_attempted.load());
 assert(sv::direct_heap::state.cache_passed.load()!=negative);
 unsigned complete=0,fail=0,bench=0,masks=0;
 for(const auto&r:rows){if(r.name=="controls_complete"){++complete;assert((r.status=="pass")!=negative);}
   if(r.status=="fail")++fail;if(r.name.find("private_ABBA_")==0)++bench;
   if(r.name.find("cleanup_mask_")==0 && r.status=="pass")++masks;}
 assert(negative || masks==16);assert(sv::direct_heap::state.cache_finished.load());
 assert(complete==1);assert(negative?fail>0:fail==0);assert(negative?bench==0:bench==4);
 const auto count=rows.size();sv::run_protected_cache_probe(&test_hash);assert(rows.size()==count);
 assert(sv::heap_fault_safe_to_chainload());
}
'''

def recipe_regression():
    # Execute the actual engine against tiny Git blobs: ordered same-file
    # extensions, all-files validation before writing, idempotence and bad pins.
    with tempfile.TemporaryDirectory() as d:
        root=Path(d);(root/'scripts').mkdir();(root/'patches/experiments').mkdir(parents=True)
        script=root/'scripts/apply-switch-write-watch-hooks.py'
        shutil.copyfile(ROOT/'scripts/apply-switch-write-watch-hooks.py',script)
        subprocess.run(['git','init','-q',str(root)],check=True)
        original=b'base\n';other=b'other\n'
        def blob(data):
            return subprocess.run(['git','hash-object','-w','--stdin'],input=data,cwd=root,check=True,capture_output=True).stdout.decode().strip()
        sha=blob(original);sha2=blob(other)
        def spec(path,pin,old,new):return dict(path=path,base_blob=pin,replacements=[dict(old=old,new=new)])
        # Use the engine's actual ordered list rather than silently limiting
        # the fixture to the four recipes that existed when it was written.
        engine=load(script,'fixture_recipe_engine')
        recipes=list(engine.RECIPES)
        assert len(recipes)>=3 and all(p.parent==root/'patches/experiments' for p in recipes)
        specs=[[spec('one',sha,'base','middle')],
               [spec('two',sha2,'other','final')],
               [spec('one',sha,'middle','done')]]
        specs.extend([] for _ in recipes[3:])
        for path,files in zip(recipes,specs):
            path.write_text(json.dumps(dict(files=files)))
        (root/'one').write_bytes(original);(root/'two').write_bytes(b'unknown\n')
        def run(*args):return subprocess.run(['python3',str(script),*args],cwd=root,capture_output=True)
        def expect_success(*args):
            result=run(*args)
            assert result.returncode==0,result.stderr.decode(errors='replace')
        assert run().returncode and (root/'one').read_bytes()==original
        (root/'two').write_bytes(other)
        # A missing required recipe must fail before mutating any source, too.
        last=recipes[-1];saved=last.read_bytes();last.unlink()
        assert run().returncode and (root/'one').read_bytes()==original
        assert (root/'two').read_bytes()==other
        last.write_bytes(saved)
        expect_success()
        assert (root/'one').read_bytes()==b'done\n' and (root/'two').read_bytes()==b'final\n'
        expect_success();expect_success('--check-applied')
        provenance=json.loads((root/'build-switch-preflight/write-watch-hooks.json').read_text())
        assert [r['path'] for r in provenance['recipes']]==[p.relative_to(root).as_posix() for p in recipes]
        recipes[2].write_text(json.dumps(dict(files=[spec('one',sha2,'middle','done')])))
        assert run().returncode and (root/'one').read_bytes()==b'done\n'
    print('Recipe engine: complete ordered list, missing-file transaction, pin rejection and idempotence passed')

def main():
    p=argparse.ArgumentParser();p.add_argument('--cxx',default='c++');p.add_argument('--sanitize',action='store_true');a=p.parse_args()
    recipe_regression()
    load(ROOT/'scripts/tests/test_heap_permission_runs.py','permission_runs').run(a.cxx,a.sanitize)
    old=load(ROOT/'scripts/tests/test_switch_heap_fault.py','heap_fixture')
    stub=old.STUB.replace('uint32_t type=0,attr=0,perm=0;', 'uint32_t type=0,attr=0,perm=0,ipc_refcount=0,device_refcount=0;')
    with tempfile.TemporaryDirectory() as directory:
        tmp=Path(directory);(tmp/'switch.h').write_text(stub)
        common=[a.cxx,'-std=c++20','-O2','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-Wno-missing-field-initializers',
            '-D__SWITCH__','-DVITA3K_SWITCH_TRACELAB=1','-I',str(tmp),'-I',str(ROOT/'vita3k/util/include')]
        if a.sanitize:common+=['-fsanitize=address,undefined','-fno-omit-frame-pointer']
        (tmp/'util').mkdir(); (tmp/'switch/arm').mkdir(parents=True)
        (tmp/'util/log.h').write_text('#pragma once\n#define LOG_INFO(...) ((void)0)\n#define LOG_WARN(...) ((void)0)\n#define LOG_ERROR(...) ((void)0)\n')
        (tmp/'switch/arm/counter.h').write_text('#pragma once\n#include <cstdint>\ninline uint64_t armGetSystemTickFreq(){return 1000;}\n')
        (tmp/'policy.cpp').write_text('#include <malloc.h>\n'+POLICY)
        subprocess.run(common+[str(tmp/'policy.cpp'),'-o',str(tmp/'policy')],check=True)
        subprocess.run([str(tmp/'policy'),str(tmp)],check=True,timeout=20)
        topology=json.loads((tmp/'topology.json').read_text());assert topology['game_hash_skipping'] is False
        (tmp/'cache.cpp').write_text('#include <algorithm>\n'+CACHE.replace('PLATFORM_SOURCE',str(ROOT/'vita3k/util/src/switch_heap_fault_probe.cpp')))
        if (ROOT/'external/xxHash/xxhash.h').is_file():
            hashing=['-DUSE_PINNED_XXHASH','-I',str(ROOT/'external/xxHash')]
        elif ctypes.util.find_library('xxhash'):
            hashing=['-lxxhash']
        else:
            raise RuntimeError('Need pinned xxHash checkout or host libxxhash; never silently substitute a digest')
        subprocess.run(common+[str(tmp/'cache.cpp'),*hashing,'-o',str(tmp/'cache')],check=True)
        for case in ('positive','negative'):
            subprocess.run([str(tmp/'cache'),str(tmp),case],check=True,timeout=20)
        controller=old.CONTROLLER.replace('#include <cassert>', '#include <util/switch_direct_heap.h>\n#include <cassert>')
        anchor=' sv::begin_session("NO_CONFIG");'
        extra=r'''
 sv::direct_heap::state.active.store(true);sv::direct_heap::state.commits.store(3);sv::direct_heap::state.pool_bytes.store(768ull<<20);
 sv::begin_session("DIRECT");
 sv::record("heap_fault","native_suite_complete","pass","controller fixture");
 sv::record("heap_fault","jit_suite_complete","pass","controller fixture");
 sv::direct_heap::state.cache_passed.store(true);sv::direct_heap::state.cache_finished.store(true);
 sv::direct_heap::state.cache_cleanup_ok.store(true);
 sv::on_render_end(1);assert(sv::status_snapshot().stage==sv::Stage::Waiting);
 assert(std::strstr(sv::make_hud_text(sv::status_snapshot(),1).title.data(),"WAITING FOR SCENE"));
 sv::request_benchmark();
 sv::on_render_end(100000);assert(sv::status_snapshot().stage==sv::Stage::Measuring);
 sv::direct_heap::state.ranges.store(4);
 assert(sv::status_snapshot().remaining_ms==30000);
 assert(std::strstr(sv::make_hud_text(sv::status_snapshot(),100000).title.data(),"GAME VALIDATION"));
 for(unsigned n=1;n<=120;++n)sv::on_render_end(100000+n*250);
 auto direct=sv::status_snapshot();assert(direct.stage==sv::Stage::Complete&&direct.outcome==sv::CaptureOutcome::Ready);
 sv::frontend_poll(true);assert(sv::status_snapshot().stage==sv::Stage::Complete);
 std::filesystem::create_directory("direct-report");sv::write_report("direct-report");sv::end_session();
 sv::begin_session("SCRATCH_FAILURE");
 sv::direct_heap::state.cache_finished.store(true);sv::direct_heap::state.cache_passed.store(false);
 sv::on_render_end(2);assert(sv::status_snapshot().stage==sv::Stage::Complete);
 assert(sv::status_snapshot().outcome==sv::CaptureOutcome::Invalid);
 sv::request_benchmark();assert(!sv::direct_heap::state.start_requested.load());sv::end_session();
 sv::direct_heap::state.active.store(false);
'''
        assert controller.count(anchor)==1;controller=controller.replace(anchor,extra+anchor)
        (tmp/'controller.cpp').write_text(controller)
        subprocess.run(common+['-pthread','-DVITA3K_SWITCH_HEAP_FAULT_DEFAULT=1','-DVITA3K_SWITCH_CLEAN_CAPTURE_DEFAULT=1',
            '-DVITA3K_SWITCH_WRITE_SHADOW_DEFAULT=1',str(tmp/'controller.cpp'),str(ROOT/'vita3k/util/src/switch_memory_validation.cpp'),
            '-o',str(tmp/'controller')],check=True)
        subprocess.run([str(tmp/'controller'),str(tmp)],check=True,timeout=20)
        report=json.loads((tmp/'direct-report/memory-validation.json').read_text())
        assert report['capture_plan']=='heap-fault-4-deferred' and report['guest_backing']['frames']==121
        analyzer=load(ROOT/'scripts/analyze-switch-capture.py','direct_analyzer')
        # A fake summary bit without the individual controls is never evidence.
        assert not analyzer.analyze(report)['protected_cache_controls_passed']
    cross=shutil.which('aarch64-none-elf-g++')
    if cross:
        with tempfile.TemporaryDirectory() as d:
            t=Path(d); (t/'headers.cpp').write_text('#include <util/switch_direct_heap_platform.h>\n#include <util/switch_protected_cache_probe.h>\nvoid check(switch_validation::ProtectedHash h){switch_validation::run_protected_cache_probe(h);switch_validation::direct_heap::requested();}\n')
            sdk=Path(os.environ.get('DEVKITPRO','/opt/devkitpro'))
            subprocess.run([cross,'-std=c++20','-O2','-fno-lto','-D_GNU_SOURCE','-D__SWITCH__','-DVITA3K_SWITCH_TRACELAB=1',
                '-march=armv8-a+crc+crypto','-mtune=cortex-a57','-mtp=soft','-ftls-model=local-exec',
                '-I',str(sdk/'libnx/include'),'-I',str(ROOT/'vita3k/util/include'),'-c',str(t/'headers.cpp'),'-o',str(t/'headers.o')],check=True)
        print('Real devkitA64/libnx header compile passed (not execution)')
    else:
        print('Real devkitA64 header compile not run: SDK unavailable')
    print('Direct Heap policy/HUD, real C++ fault handler + POSIX retry, XXH3 cache controls and broken-evidence negative control passed; NOT Horizon/A64 or live Guest validation')
if __name__=='__main__':main()
