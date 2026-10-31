#!/usr/bin/env python3
"""Scene-cadence integration, stable collision keys and real inserted contracts.
Host adapters are explicit. This is not native GXM/Vulkan/device execution.
"""
import argparse, importlib.util, json, re, subprocess, sys, tempfile, signal
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]

def load(path,name):
 s=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(s);s.loader.exec_module(m);return m

def main():
 p=argparse.ArgumentParser();p.add_argument('--cxx',default='c++');p.add_argument('--sanitize',action='store_true');p.add_argument('--require-source',action='store_true');a=p.parse_args()
 engine=load(ROOT/'scripts/apply-switch-write-watch-hooks.py','scene_engine')
 recipe=json.loads((ROOT/'patches/experiments/switch-scene-reuse.json').read_text())
 if a.require_source:
  subprocess.run([sys.executable,str(ROOT/'scripts/apply-switch-write-watch-hooks.py'),'--check-applied'],check=True)
  for path,sha in recipe['reviewed_dependencies'].items():
   blob=subprocess.check_output(['git','show','HEAD:'+path],cwd=ROOT)
   assert engine.blob_sha(blob)==sha,('changed reviewed dependency',path)
 # Check this stage against its predecessor, then independently check the full
 # final chain. Future extensions must not be mistaken for rollback damage.
 for f in recipe['files']:
  original=subprocess.check_output(['git','cat-file','blob',f['base_blob']],cwd=ROOT)
  prior=original
  for q in engine.RECIPES:
   if q.name=='switch-scene-reuse.json':break
   for item in json.loads(q.read_text())['files']:
    if item['path']==f['path']:prior=engine.transform(prior,item['replacements'])
  result=engine.transform(prior,f['replacements']);inverse=result.decode()
  for op in reversed(f['replacements']):
   assert inverse.count(op['new'])==1;inverse=inverse.replace(op['new'],op['old'],1)
  assert inverse.encode()==prior
  final=original
  for q in engine.RECIPES:
   for item in json.loads(q.read_text())['files']:
    if item['path']==f['path']:final=engine.transform(final,item['replacements'])
  assert (ROOT/f['path']).read_bytes()==final
 # Allocation zeroing is protected by the existing mapping lifetime lock;
 # callback memcpy has an independent pre-write lease before queue publication.
 alloc=(ROOT/'vita3k/mem/src/mem.cpp').read_text()
 alloc=alloc[alloc.index('static Address alloc_inner(',alloc.index('static Address alloc_inner(')+1):]
 assert alloc.index('MappingGuard audit_mapping')<alloc.index('std::memset(run_host')<alloc.index('publish_page_entry(state, gp')
 gxm=(ROOT/'vita3k/modules/SceGxm/SceGxm.cpp').read_text()
 dq=gxm[gxm.index('EXPORT(int, sceGxmDisplayQueueAddEntry,'):gxm.index('EXPORT(int, sceGxmDisplayQueueFinish)')]
 assert dq.index('declare_outputs')<dq.index('const Address address = alloc(')<dq.index('HostOutputLease callback_copy')<dq.index('memcpy(ptr.get')<dq.index('display_queue.push')
 def snippet(needle,index=0):
  ops=[op for f in recipe['files'] for op in f['replacements'] if needle in op['old']]
  assert len(ops)==1,(needle,len(ops))
  blocks=re.findall(r'#if defined\(__SWITCH__\) && defined\(VITA3K_SWITCH_TRACELAB\)\n(.*?)\n#endif',ops[0]['new'],re.S)
  return blocks[index]
 begin=snippet('context->state.fragment_sync_object = fragmentSyncObject;')
 end=snippet('SceGxmNotification empty_notification =')
 descriptor=snippet('TRACY_FUNC(sceGxmTextureSetUAddrModeSafe,')
 queue=snippet('const Address address = alloc(emuenv.mem, emuenv.gxm.params.displayQueueCallbackDataSize')
 ctrl=snippet('const size_t element_size = is_v2 ? sizeof(SceCtrlData2) : sizeof(SceCtrlData);')
 touch=snippet('int touch_get(const SceUID thread_id,')
 # Execute the exact new blocks using the same real Domain as the controller.
 extra=r'''
#include <util/gxm_range_contract.h>
namespace renderer {struct Command { uint8_t bytes[64]{}; };}
constexpr int SCE_GXM_CONTEXT_TYPE_IMMEDIATE=1;
struct SceGxmSyncObject { uint64_t timestamp=0; };
template<class T>struct ScenePtr { T *p=nullptr; uintptr_t address()const{return reinterpret_cast<uintptr_t>(p);} T*get(const MemState&)const{return p;} explicit operator bool()const{return p!=nullptr;} };
struct SceGxmContext {
 struct {int type=1;bool active=false;ScenePtr<SceGxmSyncObject>fragment_sync_object;}state;
 uint32_t command_allocator_size=16;ScenePtr<uint8_t>alloc_space,alloc_space_end;
};
static void scene_boundary(const MemState &mem) {
 struct Env{const MemState &mem;}emuenv{mem};
 static std::array<uint8_t,16*sizeof(renderer::Command)>ring{};
 static SceGxmSyncObject sync,other;
 static SceGxmContext object{{1,false,{&sync}},16,{ring.data()},{ring.data()+ring.size()}};
 auto *context=&object;
 {sv::epoch::HostLease root;sv::epoch::name_import("sceGxmBeginScene");
 // BEGIN_CONTRACT
 context->state.active=true;ring[0]^=1;}
 {sv::epoch::HostLease root;sv::epoch::name_import("sceGxmEndScene");
 // END_CONTRACT
 context->state.active=false;++sync.timestamp;ring.back()^=1;}
 SceGxmTexture descriptor{};auto *texture=&descriptor;
 {sv::epoch::HostLease root;sv::epoch::name_import("sceGxmTextureSetUAddrModeSafe");
 // DESCRIPTOR_CONTRACT
 texture->fmt=4;}
 {sv::epoch::HostLease root;sv::epoch::name_import("sceGxmDisplayQueueAddEntry");
 ScenePtr<SceGxmSyncObject> oldBuffer{&sync},newBuffer{&other};
 // QUEUE_CONTRACT
 std::array<uint8_t,32>payload{};
 {sv::epoch::HostOutputLease copy(payload.data(),{payload.size(),true});payload[0]=9;}
 ++sync.timestamp;++other.timestamp;}
}
'''
 extra=extra.replace('// BEGIN_CONTRACT',begin).replace('// END_CONTRACT',end).replace('// DESCRIPTOR_CONTRACT',descriptor).replace('// QUEUE_CONTRACT',queue)
 # Use the production scene-memo predicate verbatim. Audit and performance now
 # see one content decision per scene, not four warm repeated calls per loop.
 cache=(ROOT/'vita3k/renderer/src/texture/cache.cpp').read_text()
 start=cache.index('            verified_this_scene = !strict_hash');endline=cache.index(';',start)+1
 predicate=cache[start:endline]
 body=load(ROOT/'scripts/tests/test_switch_resource_integration.py','base_resource')
 source=body.CPP.replace('// PRODUCTION_HASH_BODY',extra+'\n// PRODUCTION_HASH_BODY')
 old='auto get=[&](bool excluded=false){SceGxmTexture texture{(excluded?12288u:8192u)>>2,4};return hash_texture_data(texture,4096,mem);};'
 new=r'''uint64_t memo_scene=1;struct Memo{uint64_t last_hashed_scene=0,hash=0;};std::array<Memo,2>memo{};
 auto get=[&](bool excluded=false){auto*info=&memo[excluded?1:0];const bool strict_hash=sv::strict_hash();
 const bool import_textures=false,export_textures=false;bool verified_this_scene=false;
 // MEMO_PREDICATE
 if(verified_this_scene)return info->hash;
 info->last_hashed_scene=memo_scene;SceGxmTexture texture{(excluded?12288u:8192u)>>2,4};
 return info->hash=hash_texture_data(texture,4096,mem);};'''.replace('// MEMO_PREDICATE',predicate)
 assert old in source;source=source.replace(old,new)
 source=source.replace('assert(!sv::strict_hash()); // all modes use the production scene memo','assert(!sv::strict_hash()); // same production cadence\n  ++memo_scene;scene_boundary(mem);')
 # Count reuse only on first scene lookup. Existing memo hits are not new reuse.
 source=source.replace('if(reads==before){assert(phase==2||phase==3);assert(!saw_veto);saw_skip=true;}','if(reads==before && k==0 && !(phase==3&&n==5)){assert(phase==2||phase==3);assert(!saw_veto);saw_skip=true;}')
 # Every scenario still executes the actual production controller, Hash function,
 # Table, cache, HUD and JSON analyzer. Network adapters are inherited unchanged.
 body.CPP=source
 body.main()
 if not a.sanitize:
  # Reintroduce the previous unscoped scene boundaries. It must compile and
  # fail the real once-per-scene controller's usable-treatment assertion.
  body.CPP=source.replace(begin,'/* deliberately unscoped BeginScene */').replace(end,'/* deliberately unscoped EndScene */')
  try: body.main(); raise AssertionError('Unscoped scene mutation escaped')
  except subprocess.CalledProcessError as e: assert e.returncode==-signal.SIGABRT,('negative must fail at runtime',e)
  body.CPP=source

 # A separate small real-header test exercises colliding inputs, no-delete probe
 # chains, full capacity and invalidating a baseline between once-per-scene visits.
 unit=r'''
#include <util/resource_hash_reuse.h>
#include <util/gxm_range_contract.h>
#include <cassert>
#include <vector>
namespace ep=switch_validation::epoch;
struct Memory{};
struct Pointer{uint64_t a=0;uint8_t*p=nullptr;uint64_t address()const{return a;}uint8_t*get(const Memory&)const{return p;}};
struct Context{struct{int type=1;}state;uint64_t command_allocator_size=2;Pointer alloc_space,alloc_space_end;};
void exact_output_bounds(){
 ep::enabled=true;Memory mem;std::array<uint8_t,512>bytes{};
 Context ctx{{1},2,{4096,bytes.data()},{4224,nullptr}};
 ep::Domain d;d.watch_range(0,bytes.data(),128,nullptr,0);d.watch_range(1,bytes.data()+128,8,nullptr,0);d.watch_range(2,bytes.data()+136,1,nullptr,0);auto serial=d.publish();
 {ep::HostLease lease(&d,11);ep::name_import("end");assert(ep::declare_gxm_context(&ctx,mem,{"end"},true,1,64,{bytes.data()+128,8}));
  assert(d.stamp(serial,0).ranges.busy&&d.stamp(serial,1).ranges.busy&&!d.stamp(serial,2).ranges.busy);}
 {ep::HostLease lease(&d,12);ep::name_import("end");ep::HostSpan invalid;invalid.begin=UINTPTR_MAX-1;invalid.size=8;
  assert(!ep::declare_gxm_context(&ctx,mem,{"end"},true,1,64,invalid));assert(d.stamp(serial,0).ranges.unknown_busy);}
 for(uint32_t count:{0u,1u,2u})for(size_t element_size:{32u,40u}){
  ep::Domain output;auto*pData=bytes.data();size_t actual=element_size*(count?count:1u);
  output.watch_range(0,bytes.data(),actual,nullptr,0);output.watch_range(1,bytes.data()+actual,1,nullptr,0);auto token=output.publish();
  {ep::HostLease lease(&output,13);ep::name_import("sceCtrlReadBufferPositive");
   // CTRL_OUTPUT_BLOCK
   assert(output.stamp(token,0).ranges.busy==1&&!output.stamp(token,1).ranges.busy&&!output.stamp(token,1).ranges.unknown_busy);}
  assert(!output.stamp(token,0).ranges.busy);
 }
 struct TouchData{uint8_t data[40];};std::array<TouchData,3>data{};
 for(uint32_t count:{0u,1u,2u}){
  ep::Domain output;auto*pData=data.data();auto actual=sizeof(*pData)*count;
  output.watch_range(0,pData,actual,nullptr,0);output.watch_range(1,reinterpret_cast<uint8_t*>(pData)+actual,1,nullptr,0);auto token=output.publish();
  {ep::HostLease lease(&output,14);ep::name_import("sceTouchPeek");
   // TOUCH_OUTPUT_BLOCK
   assert(!output.stamp(token,1).ranges.busy&&!output.stamp(token,1).ranges.unknown_busy);
   if(count)assert(output.stamp(token,0).ranges.busy==1);}
 }
 ep::enabled=false;
}
using namespace switch_validation::reuse;
size_t bucket(const Input&i){auto x=uint64_t(i.pixels)^(uint64_t(i.palette)<<1)^i.pixel_bytes^(i.palette_bytes<<17)^(i.hash_domain<<7);x^=x>>33;x*=0xff51afd7ed558ccdULL;x^=x>>33;return x&63;}
int main(){
 exact_output_bounds();
 HashCache<64>cache;Evidence proof;proof.valid=proof.publication_drained=proof.gpu_host_visible=true;proof.allocation=proof.lifetime=proof.mapping=proof.publication=1;proof.coverage=CompleteCoverage;
 std::vector<Input>keys;for(uintptr_t p=4096;keys.size()<65;p+=4096){Input i{p,0,128,0,1};if(bucket(i)==0)keys.push_back(i);}
 unsigned reads=0;auto eval=[&](unsigned i){return cache.evaluate(keys[i],[&]()noexcept{return proof;},[&]{++reads;return uint64_t(i+123);});};
 cache.begin();cache.phase(1,Mode::Audit);for(unsigned i=0;i<64;++i){assert(eval(i)==i+123);assert(eval(i)==i+123);}
 cache.phase(2,Mode::Performance);auto n=reads;for(unsigned scene=0;scene<20;++scene)for(unsigned i=0;i<64;++i)assert(eval(i)==i+123);assert(reads==n);
 // A full table declines a new key, never destroying the other audit proofs.
 assert(eval(64)==187);assert(reads==n+1);assert(cache.report().total.fallback[size_t(Reason::Capacity)]==1);
 proof.valid=false;eval(0);proof.valid=true;n=reads;assert(eval(63)==186&&reads==n); // no hole in chain
 eval(0);n=reads;eval(0);assert(reads==n+1); // revoked audit needs fresh Audit, not restored bits
 // Unknown write every scene correctly forces full Hash, even on equal bytes.
 n=reads;for(unsigned scene=0;scene<20;++scene){++proof.unknown;for(unsigned i=1;i<64;++i)eval(i);}assert(reads==n+20*63);
 assert(cache.report().total.version_fields[5]>=20*63);
}
'''
 unit=unit.replace('// CTRL_OUTPUT_BLOCK',ctrl).replace('// TOUCH_OUTPUT_BLOCK',touch)
 with tempfile.TemporaryDirectory() as tmp:
  d=Path(tmp);(d/'case.cpp').write_text(unit)
  cmd=[a.cxx,'-std=c++20','-O0' if a.sanitize else '-O2','-Wall','-Wextra','-Werror','-pthread','-I'+str(ROOT/'vita3k/util/include'),str(d/'case.cpp'),'-o',str(d/'case')]
  if a.sanitize:cmd+=['-fsanitize=address,undefined','-fno-omit-frame-pointer']
  subprocess.run(cmd,check=True,timeout=90);subprocess.run([str(d/'case')],check=True,timeout=30)
  if not a.sanitize:
   # Replay the exact previous direct-map header, not a hand-written model.
   legacy=subprocess.check_output(['git','cat-file','blob','aa4a0bf3cf786eb4a3848a9086d9247981514673'],cwd=ROOT)
   old=d/'old/util';old.mkdir(parents=True);(old/'resource_hash_reuse.h').write_bytes(legacy)
   (d/'case.cpp').write_text(unit.replace(' assert(cache.report().total.version_fields[5]>=20*63);',''))
   prior_cmd=cmd.copy();prior_cmd.insert(1,'-I'+str(d/'old'))
   subprocess.run(prior_cmd,check=True,timeout=90)
   result=subprocess.run([str(d/'case')],capture_output=True,timeout=30)
   assert result.returncode==-signal.SIGABRT and b'Assertion' in result.stderr,('old direct-map cache must fail',result.stderr)

 print('PASS: identical production scene-memo cadence in all modes; actual inserted Begin/End/descriptor/display-queue contracts; once-per-scene full controller/Hash/Table audit and reuse; 64 same-bucket keys, saturation and invalidated-probe-chain safety; exact pinned source replay. Host adapters, not native GXM/GPU.')
if __name__=='__main__':main()
