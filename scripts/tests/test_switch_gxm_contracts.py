#!/usr/bin/env python3
"""Real metadata differential tests and pinned GXM output-body integration.
Host context/Ptr adapters are explicit models, not native GPU or Guest coverage.
"""
import argparse, copy, importlib.util, json, subprocess, tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
OLD='ac869a454834774bdffe553f1a9eed07139227b9'
CPP=r'''
#include <util/gxm_range_contract.h>
#include <util/switch_write_watch.h>
#include "reference.h"
#include <cassert>
#include <array>
#include <random>
#include <thread>
#include <vector>
namespace e=switch_validation::epoch;
namespace r=switch_validation::reference;
struct Memory{};
struct Pointer { uint64_t address_=0; uint8_t *host=nullptr;
 uint64_t address() const{return address_;} uint8_t *get(const Memory&)const{return host;}};
enum class ContextType { Immediate, Deferred };
struct Context {struct {ContextType type=ContextType::Immediate;}state;uint64_t command_allocator_size=0;Pointer alloc_space,alloc_space_end;};
void contexts() {
 e::enabled=true;e::Domain d;Memory mem;Context c;std::array<uint8_t,16384> bytes{};
 c.command_allocator_size=32;c.alloc_space={4096,bytes.data()+4096};c.alloc_space_end={6144,nullptr};
 d.watch_range(0,bytes.data(),16,nullptr,0);d.watch_range(1,&c,sizeof(c),nullptr,0);
 d.watch_range(2,bytes.data()+4096,8,nullptr,0);const auto t=d.publish();
 auto before=d.stamp(t,0).ranges;
 {e::HostLease h(&d,1);e::name_import("sceGxmDraw");
  assert(e::declare_gxm_context(&c,mem,{"sceGxmDraw"},true,ContextType::Immediate,64));
  assert(e::stable_host(before,d.stamp(t,0).ranges));
  assert(d.stamp(t,1).ranges.busy==1&&d.stamp(t,2).ranges.busy==1);}
 assert(!d.stamp(t,1).ranges.busy&&!d.stamp(t,2).ranges.busy);
 // Deferred command allocation may call Guest code: never narrow it.
 c.state.type=ContextType::Deferred;
 {e::HostLease h(&d,2);e::name_import("sceGxmDraw");
  assert(!e::declare_gxm_context(&c,mem,{"sceGxmDraw"},true,ContextType::Immediate,64));assert(d.stamp(t,0).ranges.unknown_busy);}
 // Simple setters write context only, even for deferred contexts.
 {e::HostLease h(&d,3);e::name_import("setter");
  assert(e::declare_gxm_context(&c,mem,{"setter"},false,ContextType::Immediate,64));
  assert(d.stamp(t,1).ranges.busy==1&&!d.stamp(t,2).ranges.busy);}
 c.state.type=ContextType::Immediate;
 {e::HostLease h(&d,4);e::name_import("unreviewed_outer");
  assert(!e::declare_gxm_context(&c,mem,{"sceGxmDraw"},true,ContextType::Immediate,64));assert(d.stamp(t,0).ranges.unknown_busy);}
 for(unsigned kind=0;kind<4;++kind){
  Context bad=c;
  if(kind==0)bad.command_allocator_size=UINT64_MAX;
  if(kind==1)bad.alloc_space_end.address_=1;
  if(kind==2)bad.alloc_space.host=nullptr;
  if(kind==3)bad.alloc_space_end.address_=4097;
  e::HostLease h(&d,5);e::name_import("sceGxmDraw");
  assert(!e::declare_gxm_context(&bad,mem,{"sceGxmDraw"},true,ContextType::Immediate,64));}
 assert(!d.snapshot().failed);e::enabled=false;
}
void cache_invalidation(){
 e::HleRanges h;std::array<uint8_t,32768>b{};
 h.watch(0,{b.data(),4},{});
 // Populate a negative intersection and reuse it at begin/end.
 for(unsigned n=0;n<128;++n){auto s=h.enter(256);h.name(s,"setter");assert(h.declare(s,{"setter"},{{b.data()+4096,32}}));h.leave(s);}
 auto s=h.enter(256);h.name(s,"setter");assert(h.declare(s,{"setter"},{{b.data()+4096,32}}));
 h.watch(1,{b.data()+4096,4},{});assert(h.stamp(1).busy==1);h.leave(s);assert(h.stamp(1).busy==0);
 // Clear/re-register while scoped output is live; palettes count, too.
 s=h.enter(256);h.name(s,"setter");assert(h.declare(s,{"setter"},{{b.data()+4096,32}}));
 h.clear_interests();h.watch(0,{b.data(),4},{b.data()+4100,4});assert(h.stamp(0).busy==1);
 h.leave(s);assert(!h.stamp(0).busy&&!h.failed());
 auto before=h.stamp(0);s=h.enter(256);h.name(s,"setter");assert(h.declare(s,{"setter"},{{b.data()+4096,32}}));h.leave(s);
 assert(!e::stable_host(before,h.stamp(0))); // even a same-value output is an event
 FILE*f=std::fopen("cache.json","wb");assert(f);h.write_json(f);std::fclose(f);
 assert(h.reset());h.watch(0,{b.data()+4096,4},{});s=h.enter(256);h.name(s,"setter");
 assert(h.declare(s,{"setter"},{{b.data()+4096,32}}));assert(h.stamp(0).busy==1);h.leave(s);assert(!h.failed());
}
void differential(){
 e::HleRanges fast;r::HleRanges old;std::array<uint8_t,65536>b{};
 struct Live{unsigned f,o;};std::vector<Live> live;unsigned interests=0;
 std::array<e::RangeStamp,32> fs{};std::array<r::RangeStamp,32> os{};std::mt19937 gen(0x17933468);
 for(unsigned step=0;step<40000;++step){
  switch(gen()%7){
  case 0:if(live.size()<64){uint32_t nid=(gen()%300)*256;live.push_back({fast.enter(nid),old.enter(nid)});}break;
  case 1:if(!live.empty()){const auto x=live[gen()%live.size()];auto*p=b.data()+gen()%60000;auto n=gen()%64;
   fast.name(x.f,"output");old.name(x.o,"output");bool prior=gen()%2;
   assert(fast.declare(x.f,{"output"},{{p,n}},prior)==old.declare(x.o,{"output"},{{p,n}},prior));}break;
  case 2:if(!live.empty()){auto i=gen()%live.size();fast.leave(live[i].f);old.leave(live[i].o);live.erase(live.begin()+i);}break;
  case 3:if(interests<32){auto*p=b.data()+gen()%60000;auto*q=b.data()+gen()%60000;
   fast.watch(interests,{p,128},{q,8});old.watch(interests,{p,128},{q,8});++interests;}break;
  case 4:fast.gpu_boundary();old.gpu_boundary();break;
  case 5:if(gen()%8==0){fast.clear_interests();old.clear_interests();interests=0;fs={};os={};}break;
  case 6:if(!live.empty()){auto x=live[gen()%live.size()];bool f=fast.park(x.f,"output"),o=old.park(x.o,"output");assert(f==o);
   if(f){fast.resume(x.f);old.resume(x.o);}}break;
  }
  assert(fast.failed()==old.failed()&&!fast.failed());
  for(unsigned i=0;i<interests;++i){auto a=fast.stamp(i);auto z=old.stamp(i);
   assert(a.valid==z.valid&&a.unknown==z.unknown&&a.busy==z.busy&&a.unknown_busy==z.unknown_busy&&a.gpu==z.gpu);
   assert(e::stable_host(fs[i],a)==r::stable_host(os[i],z));fs[i]=a;os[i]=z;}
 }
 for(auto x:live){fast.leave(x.f);old.leave(x.o);}assert(!fast.failed());
}
void byte_counts(){
 namespace w=switch_validation::watch;e::Domain d;w::Table t;t.set_epoch_domain(&d);
 auto raw=std::make_unique<uint8_t*[]>(w::page_count);alignas(4096)std::array<uint8_t,4096>b{};
 raw[2]=reinterpret_cast<uint8_t*>(uintptr_t(b.data())-8192);assert(t.bind(raw.get()));
 t.phase(0,false,true);auto hash=[&]{return t.after(t.before(raw.get(),8192,b.data(),4,b.data()+128,4),0);};hash();
 t.phase(1,true,false,true);hash();hash();auto c=t.snapshot().phases[1];
 assert(c.cpu_candidates==1&&c.cpu_candidate_bytes==8&&c.hle_candidate_bytes==8&&c.submission_candidate_bytes==8);
 t.phase(w::no_index,false);t.detach(raw.get());
}
int main(){contexts();cache_invalidation();differential();byte_counts();}
'''
def load(path,name):
 s=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(s);s.loader.exec_module(m);return m

def source_gate(required):
 recipe=json.loads((ROOT/'patches/experiments/switch-gxm-contracts.json').read_text())
 if required:
  subprocess.run(['python3',str(ROOT/'scripts/apply-switch-write-watch-hooks.py'),'--check-applied'],check=True)
  for f in recipe['files']:
   text=(ROOT/f['path']).read_text()
   for op in f['replacements']:assert op['new'] in text, f['path']
 # Deferred callbacks and the GPU lifecycle are not converted into no-write APIs.
 assert len(recipe['files'][0]['replacements'])==15
 assert 'sceGxmDrawPrecomputed' not in json.dumps(recipe['files'][0])
 return recipe

def report_gate(d,cmd,recipe):
 # Compile the actual production serialization addition, not a handwritten JSON copy.
 op=next(f for f in recipe['files'] if f['path'].endswith('switch_memory_validation.cpp'))['replacements'][0]
 code='''#include <util/switch_write_watch.h>\n#include <cstdio>\n#include <cassert>\nint main(){
 using U=unsigned long long; switch_validation::watch::Snapshot observed;
 for(unsigned i=0;i<4;++i){auto&c=observed.phases[i];c.hash_bytes=80;
 if(i==1||i==2){c.cpu_candidate_bytes=64;c.hle_candidate_bytes=48;c.submission_candidate_bytes=32;}}
 FILE*file=std::fopen("work.json","wb");assert(file);std::fputs("{\\\"range_phases\\\":[",file);
 '''+op['new']+'\nstd::fputs("]}",file);std::fclose(file);}\n'
 (d/'report.cpp').write_text(code);subprocess.run(cmd+[str(d/'report.cpp'),'-o',str(d/'report')],check=True);subprocess.run([str(d/'report')],cwd=d,check=True)
 work=json.loads((d/'work.json').read_text())['candidate_work'];cache=json.loads((d/'cache.json').read_text())
 a=load(ROOT/'scripts/analyze-switch-epoch.py','gxm_analyzer')
 e=dict(candidate_work=work,range_contracts=cache,active_host_at_export=0,
        phases=[dict(cpu_candidates=8 if i in (1,2) else 0) for i in range(4)],
        range_phases=[dict(hle_candidates=6 if i in (1,2) else 0,submission_candidates=4 if i in (1,2) else 0) for i in range(4)])
 w=dict(phases=[dict(hash_bytes=80) for _ in range(4)]);issues=[];o=a.analyze_candidate_work(e,w,issues);assert not issues,issues
 assert o['treatment_totals']['hle_bytes']==96 and not o['safe_to_skip_hash']
 for field,value in [('hle_bytes',81),('cpu_bytes',0),('submission_bytes',49),('hash_bytes',79)]:
  bad=copy.deepcopy(e);bad['candidate_work']['phases'][1][field]=value;issues=[];a.analyze_candidate_work(bad,w,issues);assert issues,field
 for field,value in [('nid_hits',cache['calls']+1),('range_scans',0),('interests_revision',0),('content_reuse_authority',True)]:
  bad=copy.deepcopy(e);bad['range_contracts']['metadata_cache'][field]=value;issues=[];a.analyze_candidate_work(bad,w,issues);assert issues,field
 for section in ('phases','range_phases'):
  bad=copy.deepcopy(e);bad[section]=[];issues=[];a.analyze_candidate_work(bad,w,issues);assert issues
 for section in ('candidate_work','range_contracts'):
  bad=copy.deepcopy(e)
  (bad[section] if section=='candidate_work' else bad[section]['metadata_cache'])['version']=True
  issues=[];a.analyze_candidate_work(bad,w,issues);assert issues
 assert cache['metadata_cache']['nid_hits']>=127 and cache['metadata_cache']['no_overlap_hits']>=254
 issues=[];assert a.analyze_candidate_work({},w,issues) is None and not issues

def main():
 p=argparse.ArgumentParser();p.add_argument('--cxx',default='c++');p.add_argument('--sanitize',action='store_true');p.add_argument('--require-source',action='store_true');a=p.parse_args()
 recipe=source_gate(a.require_source)
 with tempfile.TemporaryDirectory() as tmp:
  d=Path(tmp);old=subprocess.run(['git','cat-file','blob',OLD],cwd=ROOT,check=True,capture_output=True).stdout.decode()
  (d/'reference.h').write_text(old.replace('switch_validation::epoch','switch_validation::reference'))
  (d/'test.cpp').write_text(CPP)
  cmd=[a.cxx,'-std=c++20','-O1' if a.sanitize else '-O2','-Wall','-Wextra','-Werror','-pthread','-I',str(ROOT/'vita3k/util/include'),'-I',str(d)]
  if a.sanitize:cmd+=['-fsanitize=address,undefined','-fno-omit-frame-pointer']
  subprocess.run(cmd+[str(d/'test.cpp'),'-o',str(d/'test')],check=True);subprocess.run([str(d/'test')],cwd=d,check=True,timeout=45)
  report_gate(d,cmd,recipe)
 print('GXM frontend/metadata PASS: context+full VDM spans, deferred/nested/malformed exclusions, late palette registration, reset, 40000 old/new semantic transitions, NID collisions/saturation, real observer byte counts, actual serializer/analyzer negatives; host only')
if __name__=='__main__':main()
