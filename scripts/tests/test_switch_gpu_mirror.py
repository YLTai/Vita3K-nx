#!/usr/bin/env python3
"""Ownership and real control-driver tests. Host GPU adapter is explicitly fake.
The device backend performs real Vulkan commands; CI checks its actual SDK syntax.
"""
import argparse
import importlib.util
import copy
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import sys
ROOT = Path(__file__).resolve().parents[2]
CPP = r'''
#include <util/mirror_probe_controls.h>
#include <cassert>
#include <random>
#include <array>
#include <cstring>
#include <algorithm>
struct Backend {
 std::array<uint8_t,16384> host{},gpu{};bool drop_upload=false, stale_read=false;
 uint8_t*cpu(){return host.data();}
 bool upload(size_t o,size_t n){if(!drop_upload)std::memcpy(gpu.data()+o,host.data()+o,n);return true;}
 bool download(size_t o,size_t n){std::memcpy(host.data()+o,gpu.data()+o,n);return true;}
 bool cpu_write(size_t o,size_t n,uint8_t v){std::memset(host.data()+o,v,n);return true;}
 bool gpu_fill(size_t o,size_t n,uint32_t v){for(size_t i=o;i<o+n;i+=4)std::memcpy(gpu.data()+i,&v,4);return true;}
 bool read_gpu(uint8_t*out){std::memcpy(out,gpu.data(),gpu.size());if(stale_read)out[23]^=1;return true;}
 bool image_roundtrip(uint8_t*out){return read_gpu(out);}
};
int main(){
 using switch_validation::mirror::PageState;using switch_validation::mirror::Owner;
 Backend b;switch_validation::mirror::Controls<Backend> controls(b);
 for(unsigned s=0;s<13;++s)assert(controls.step(s));
 assert(controls.audits>=13);
 // Independent-byte oracle must reject a skipped upload and stale readback.
 Backend lost;lost.drop_upload=true;switch_validation::mirror::Controls<Backend> broken(lost);assert(!broken.step(0));
 Backend stale;switch_validation::mirror::Controls<Backend> broken2(stale);assert(broken2.step(0));stale.stale_read=true;assert(!broken2.step(1));
 // Small model checks randomized offset/length and ownership
 // transitions without conflating content equality with a CPU write event.
 constexpr size_t N=4,P=16,S=N*P;std::mt19937 rng(0x4d495252);
 for(unsigned trial=0;trial<512;++trial){
  PageState<N,P> p;std::array<uint8_t,S> cpu{},gpu{},expected{};
  for(size_t i=0;i<S;++i)cpu[i]=expected[i]=uint8_t(rng());
  auto up=[&](size_t o,size_t n){std::memcpy(gpu.data()+o,cpu.data()+o,n);return true;};
  auto down=[&](size_t o,size_t n){std::memcpy(cpu.data()+o,gpu.data()+o,n);return true;};
  for(unsigned op=0;op<512;++op){size_t o=rng()%S,n=1+rng()%(S-o);uint8_t v=uint8_t(rng());
   if(rng()&1){assert(p.prepare_cpu_write(o,n,down));std::memset(cpu.data()+o,v,n);assert(p.commit_cpu_write(o,n));}
   else assert(p.gpu_write(o,n,up,[&](size_t a,size_t z){std::memset(gpu.data()+a,v,z);return true;}));
   std::memset(expected.data()+o,v,n);
   if((op%3)==0){assert(p.sync(0,S,true,up));assert(gpu==expected);}
   if((op%5)==0){assert(p.sync(0,S,false,down));assert(cpu==expected);}
  }
  assert(p.sync(0,S,true,up)&&p.sync(0,S,false,down));assert(cpu==expected&&gpu==expected);
 }
 PageState<4> failed;assert(!failed.sync(0,4096,true,[](size_t,size_t){return false;}));assert(!failed.valid());
 assert(!failed.commit_cpu_write(0,4));assert(!failed.sync(0,4096,false,[](size_t,size_t){return true;}));
 PageState<4> invalid;assert(!invalid.range(SIZE_MAX,1));assert(!invalid.range(16384,1));assert(invalid.range(16384,0));
 assert(invalid.gpu_write(0,4096,[](size_t,size_t){return true;},[](size_t,size_t){return true;}));
 assert(!invalid.commit_cpu_write(1,4));assert(invalid.owner(0)==Owner::Gpu); // no hidden partial overwrite
 assert(invalid.commit_cpu_write(0,4096)&&invalid.owner(0)==Owner::Cpu);
}
'''
SDK_CPP = r'''
// Match vita3k/vkutil/include/vkutil/vkutil.h on Switch. The dispatch
// table is dynamic; the desktop dlopen/LoadLibrary helper is NOT used.
#define VK_USE_PLATFORM_VI_NN
#define VK_NO_PROTOTYPES
#define VULKAN_HPP_NO_CONSTRUCTORS
#define VULKAN_HPP_NO_SPACESHIP_OPERATOR
#define VULKAN_HPP_ENABLE_DYNAMIC_LOADER_TOOL 0
#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1
#include <vulkan/vulkan.hpp>
#include <util/switch_direct_heap.h>
#include <renderer/vulkan/switch_mirror_probe.h>
struct Renderer {
 vk::Device device;vk::Queue general_queue;uint32_t general_family_index=0;
 vk::PhysicalDeviceMemoryProperties physical_device_memory;std::mutex general_queue_mutex;
 std::vector<vk::QueueFamilyProperties> physical_device_queue_families;
 vk::PhysicalDeviceProperties physical_device_properties;
 std::atomic<bool> device_lost{false};
};
void instantiate(Renderer&r){switch_validation::mirror_probe::tick(r);switch_validation::mirror_probe::shutdown(r);}
'''

CONTROLLER = r"""
#include <util/switch_direct_heap.h>
#include <util/switch_memory_validation.h>
#include <util/switch_heap_fault_probe.h>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <cstring>
int main(int argc,char**argv){
 assert(argc==2);std::filesystem::current_path(argv[1]);std::filesystem::create_directories("sdmc:/switch/vita3k");
 std::ofstream("sdmc:/switch/vita3k/tracelab.cfg")<<"mode=auto\nmemory_validation=auto\n";
 namespace sv=switch_validation;namespace m=sv::mirror_probe;namespace dh=sv::direct_heap;
 dh::state.active.store(true);sv::begin_session("GPU_FIXTURE");
 sv::record("heap_fault","native_suite_complete","pass","HOST controller fixture only");
 sv::record("heap_fault","jit_suite_complete","pass","HOST controller fixture only");
 dh::state.cache_finished.store(true);dh::state.cache_passed.store(true);dh::state.cache_cleanup_ok.store(true);
 m::state.requested.store(true);sv::on_render_end(10);
 auto view=sv::status_snapshot();assert(view.gpu_mirror&&view.stage==sv::Stage::Measuring&&view.progress_per_mille==0);
 assert(std::strstr(sv::make_hud_text(view,10).title.data(),"AUTOMATIC GPU TESTS"));
 sv::request_benchmark();assert(!dh::state.start_requested.load());
 m::state.started.store(true);m::state.completed.store(7);m::state.audits.store(7);sv::on_render_end(20);
 view=sv::status_snapshot();assert(view.stage==sv::Stage::Measuring&&view.progress_per_mille==500);
 m::state.shader_dispatches.store(10);m::state.shader_fills.store(5);m::state.shader_copies.store(5);
 m::state.shader_chains.store(1);m::state.guard_checks.store(2);
 m::state.finished.store(true);m::state.passed.store(true);m::state.cleanup.store(true);m::state.completed.store(14);
 m::state.audits.store(15);m::state.waits.store(25);m::state.uploaded.store(65536);m::state.downloaded.store(8192);m::state.faults.store(12);sv::on_render_end(30);
 view=sv::status_snapshot();assert(view.stage==sv::Stage::Complete&&view.outcome==sv::CaptureOutcome::Ready);
 assert(!dh::state.started.load());assert(std::strstr(sv::make_hud_text(view,30).action.data(),"SAVE"));
 sv::frontend_poll(true);assert(sv::status_snapshot().stage==sv::Stage::Complete);
 std::filesystem::create_directory("report");sv::write_report("report");sv::end_session();
 sv::begin_session("NEXT");assert(!m::state.requested.load()&&!m::state.finished.load());
 dh::state.cache_finished.store(true);dh::state.cache_passed.store(true);m::state.requested.store(true);
 m::state.finished.store(true);m::state.passed.store(false);sv::on_render_end(100);
 assert(sv::status_snapshot().outcome==sv::CaptureOutcome::Invalid);sv::end_session();
 sv::begin_session("PREREQUISITE_FAIL");m::state.requested.store(true);sv::record("heap_fault","native_suite_complete","fail","fixture");
 sv::on_render_end(200);assert(sv::status_snapshot().stage==sv::Stage::Complete);sv::end_session();
 std::ofstream("sdmc:/switch/vita3k/tracelab.cfg")<<"mode=off\n";sv::begin_session("OFF");assert(!sv::heap_fault_probe_enabled());sv::end_session();
}
"""

def load(path, name):
 spec=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m

def controller_gate(d,cxx,sanitize):
 (d/'util').mkdir();(d/'switch/arm').mkdir(parents=True)
 (d/'util/log.h').write_text('#pragma once\n#define LOG_ERROR(...) ((void)0)\n#define LOG_INFO(...) ((void)0)\n#define LOG_WARN(...) ((void)0)\n')
 (d/'switch/arm/counter.h').write_text('#pragma once\n#include <cstdint>\ninline uint64_t armGetSystemTickFreq(){return 1000;}\n')
 (d/'controller.cpp').write_text(CONTROLLER)
 command=[cxx,'-std=c++20','-O2','-Wall','-Wextra','-Werror','-pthread','-D__SWITCH__','-DVITA3K_SWITCH_TRACELAB=1',
  '-DVITA3K_SWITCH_HEAP_FAULT_DEFAULT=1','-I',str(d),'-I',str(ROOT/'vita3k/util/include'),
  str(d/'controller.cpp'),str(ROOT/'vita3k/util/src/switch_memory_validation.cpp'),'-o',str(d/'controller')]
 if sanitize:command+=['-fsanitize=address,undefined','-fno-omit-frame-pointer']
 subprocess.run(command,check=True);subprocess.run([str(d/'controller'),str(d)],check=True,timeout=20)
 report=json.loads((d/'report/memory-validation.json').read_text());assert report['capture_plan']=='gpu-mirror-1'
 assert report['controls_complete'] and report['capture_outcome']=='ready'
 analyzer=load(ROOT/'scripts/analyze-switch-capture.py','mirror_analyzer')
 # The aggregate flags alone NEVER make a controller fixture into device proof.
 assert analyzer.analyze(report)['exclusions']
 native={'native_store_8','native_store_16','native_store_32','native_store_64','native_store_128',
  'register_context','cross_page_128','host_memset','rearm_64','native_cleanup','native_suite_complete',
  'jit_direct_8','jit_direct_16','jit_direct_32','jit_direct_64','jit_software_alias','jit_suite_complete'}
 cache={'initial_baseline','clean_512_no_scan','pixel_first','pixel_middle','pixel_last','palette_only',
  'cross_page_store','host_memset','same_value_write','same_page_outside_input','same_address_new_lifetime',
  'post_timing_audit','controls_complete','cleanup_complete'}|{f'cleanup_mask_{i:02x}' for i in range(16)}
 gpu={'shader_program','initial_upload','clean_512_reuse','cpu_first_word','cpu_palette_word','gpu_write_while_cpu_read_only',
  'gpu_then_cpu_partial','same_value_cpu_overwrite','cross_page_cpu_write','cpu_dirty_then_gpu_partial',
  'gpu_to_cpu_read','buffer_image_roundtrip','same_address_new_lifetime','final_integrity','cleanup_complete','suite_complete'}
 report['tests']=[dict(suite=s,name=n,status='pass',detail='SYNTHETIC analyzer fixture')
  for s,names in [('heap_fault',native),('heap_cache',cache),('gpu_mirror',gpu)] for n in sorted(names)]
 assert not analyzer.analyze(report)['exclusions']
 for i in range(len(report['tests'])):
  for operation in ('remove','duplicate','fail'):
   bad=copy.deepcopy(report)
   if operation=='remove':bad['tests'].pop(i)
   elif operation=='duplicate':bad['tests'].append(bad['tests'][i])
   else:bad['tests'][i]['status']='fail'
   assert analyzer.analyze(bad)['exclusions'],(i,operation)
 for key,value in [('cleanup_complete',False),('finished',False),('completed_cases',13),('fence_waits',0),('independent_byte_audits',0),('upload_bytes',0),('download_bytes',0),('cpu_write_faults',0),('shader_dispatches',0),('shader_fill_dispatches',0),('shader_copy_dispatches',0),('shader_chain_submissions',2),('gpu_guard_audits',0),('probe_version',99)]:
  bad=copy.deepcopy(report);bad['guest_backing']['gpu_mirror'][key]=value;assert analyzer.analyze(bad)['exclusions']
 print('Actual controller/HUD/export/analyzer: no timer/button gate, failure/exit/reset/off, missing/duplicate/failed byte-control exclusions PASS (synthetic results only)')

def main():
 p=argparse.ArgumentParser();p.add_argument('--cxx',default='c++');p.add_argument('--sanitize',action='store_true');a=p.parse_args()
 with tempfile.TemporaryDirectory() as t:
  d=Path(t);(d/'test.cpp').write_text(CPP)
  cmd=[a.cxx,'-std=c++20','-O2','-Wall','-Wextra','-Werror','-pedantic','-I',str(ROOT/'vita3k/util/include'),str(d/'test.cpp'),'-o',str(d/'test')]
  if a.sanitize:cmd[2:2]=['-g','-fsanitize=address,undefined','-fno-omit-frame-pointer']
  subprocess.run(cmd,check=True);subprocess.run([str(d/'test')],check=True)
  controller_gate(d,a.cxx,a.sanitize)
  subprocess.run([sys.executable,str(ROOT/'scripts/tests/test_switch_mirror_shader.py'),'--cxx',a.cxx]+(['--sanitize'] if a.sanitize else []),check=True)
  sdk=Path(os.environ.get('DEVKITA64','/opt/devkitpro/devkitA64'))/'bin/aarch64-none-elf-g++'
  inc=ROOT/'external/VulkanMemoryAllocator-Hpp/Vulkan-Headers/include'
  if sdk.exists():
   if not (inc/'vulkan/vulkan.hpp').exists():raise RuntimeError('target Vulkan headers missing')
   generated=ROOT/'build-switch-preflight/gpu-mirror'
   subprocess.run([sys.executable,str(ROOT/'scripts/build-switch-mirror-shader.py'),'--output-dir',str(generated)],check=True)
   (d/'sdk.cpp').write_text(SDK_CPP)
   subprocess.run([str(sdk),'-std=c++20','-O2','-fno-lto','-D_GNU_SOURCE','-D__SWITCH__','-DVITA3K_SWITCH_TRACELAB=1',
     '-march=armv8-a+crc+crypto','-mtune=cortex-a57','-mtp=soft','-ftls-model=local-exec',
     '-I',str(Path(os.environ.get('DEVKITPRO','/opt/devkitpro'))/'libnx/include'),
     '-I',str(generated),'-I',str(inc),'-I',str(ROOT/'external/xxHash'),'-I',str(ROOT/'vita3k/util/include'),'-I',str(ROOT/'vita3k/renderer/include'),
     '-c',str(d/'sdk.cpp'),'-o',str(d/'sdk.o')],check=True)
   print('Actual A64/Vulkan-Hpp mirror backend compiled (not device executed)')
  else:print('SKIP target syntax gate: devkitA64 SDK unavailable locally')
 print('GPU mirror model/control driver: 262144 transitions, partial/same-value ownership, completion failure and broken-copy/readback negative controls PASS; host GPU adapter only')
if __name__=='__main__':main()
