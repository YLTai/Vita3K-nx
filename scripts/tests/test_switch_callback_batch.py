#!/usr/bin/env python3
"""Complete-empty callback proof + batched state contracts.
Local fixtures execute the real polling/wrapper bodies with explicit kernel
adapters. --require-source also replays complete pinned production files.
Not a native kernel/GPU test and never a content-reuse authorization.
"""
import argparse, copy, importlib.util, json, subprocess, tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]

def load(path,name):
    s=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(s);s.loader.exec_module(m);return m

def body(text,sig):
    b=text.index(sig);p=text.index('{',b)+1;depth=1
    while depth:
        depth+=(text[p]=='{')-(text[p]=='}');p+=1
    return text[b:p]

def sources(required):
    engine=load(ROOT/'scripts/apply-switch-write-watch-hooks.py','batch_engine')
    recipe=json.loads((ROOT/'patches/experiments/switch-callback-batch.json').read_text())
    scanner=(ROOT/'scripts/tests/fixtures/switch_callback_poll_original.cpp').read_text().strip()
    thread=next(f for f in recipe['files'] if f['path'].endswith('SceThreadmgr.cpp'))
    wrapper=thread['replacements'][0]['new']
    if required:
        chain=load(ROOT/'scripts/tests/test_switch_controller_recipe_chain.py','batch_chain')
        for f in recipe['files']:
            before,_,after=chain.replay(engine,f['path'],True)
            for op in f['replacements']:
                assert op['new'] in after.decode(),f['path']
            if f is thread:
                assert body(before.decode(),'EXPORT(SceInt32, _sceKernelWaitLwCondCB,')==thread['replacements'][0]['old']
                assert body(after.decode(),'EXPORT(SceInt32, _sceKernelWaitLwCondCB,')==wrapper
        for p,pin in recipe['reviewed_dependencies'].items():
            original=(ROOT/p).read_bytes();assert engine.blob_sha(original)==pin,p
            assert scanner==body(original.decode(),'uint32_t process_callbacks(')
        # The forwarding export must remain a pure call: no hidden Guest prefix.
        lib=(ROOT/'vita3k/modules/SceLibKernel/SceLibKernel.cpp').read_text()
        forwarding=body(lib,'EXPORT(SceInt32, sceKernelWaitLwCondCB,')
        assert forwarding=='''EXPORT(SceInt32, sceKernelWaitLwCondCB, Ptr<SceKernelLwCondWork> pWork, SceUInt32 *pTimeout) {
    TRACY_FUNC(sceKernelWaitLwCondCB, pWork, pTimeout);
    return CALL_EXPORT(_sceKernelWaitLwCondCB, pWork, pTimeout);
}'''
        driver=(ROOT/'scripts/tests/run-switch-performance-tests.sh').read_text()
        assert driver.splitlines().count('python3 "$ROOT/scripts/tests/test_switch_callback_batch.py" --cxx "${CXX:-c++}" --require-source')==1
    else:
        print('Sparse fixture mode; whole-file source/target ABI execution not claimed.')
    gxm=recipe['files'][0];assert len(gxm['replacements'])==23
    snippets=[]
    for i,op in enumerate(gxm['replacements']):
        name=op['old'].split('TRACY_FUNC(')[1].split(',')[0]
        assert op['new'].startswith(op['old']+'\n')
        snippet=op['new'][len(op['old'])+1:]
        snippets.append(f'void state_{i}(Context* context){{\n{snippet}\n}}')
    calls='\n'.join(f'{{ e::HostLease lease(&e::domain,{200+i});e::name_import("'+op['old'].split('TRACY_FUNC(')[1].split(',')[0]+f'");state_{i}(&context);assert(!e::domain.stamp(ticket,0).ranges.unknown_busy);}}' for i,op in enumerate(gxm['replacements']))
    return scanner,wrapper,'\n'.join(snippets),calls

CPP=r'''
#include <util/jit_epoch_gate.h>
#include <util/gxm_range_contract.h>
#include <util/switch_write_watch.h>
#include <cassert>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
namespace e=switch_validation::epoch;
#define LOG_WARN(...) ((void)0)
#define TRACY_FUNC(...) ((void)0)
#define EXPORT(ret,name,...) ret name(__VA_ARGS__)
using SceUID=int;using SceInt32=int;using SceUInt32=uint32_t;
struct Callback;using CallbackPtr=std::shared_ptr<Callback>;
struct Thread {bool is_processing_callbacks=false;std::vector<CallbackPtr> callbacks;};
using ThreadStatePtr=std::shared_ptr<Thread>;
struct KernelState {std::mutex mutex;ThreadStatePtr thread=std::make_shared<Thread>();
    ThreadStatePtr get_thread(SceUID){return thread;}};
struct Memory{};
struct Env {KernelState kernel;Memory mem;} emuenv;
SceUID thread_id=1;const char*export_name="_sceKernelWaitLwCondCB";
struct SceKernelLwCondWork {int uid=7;};
template<class T>struct Ptr {T*p;T*get(const Memory&)const{return p;}};
enum class SyncWeight{Light};
std::array<uint8_t,8192> texture{};
uint64_t ticket=0;bool expected_unknown=false;int expected_busy=0;unsigned dispatched=0;
struct Callback {
    bool executable=true,cancel=false,throws=false;
    bool is_executable(){return executable;}
    std::string get_name(){return "test";}
    void execute(KernelState&,std::function<void()>) {
        assert(e::domain.stamp(ticket,0).ranges.unknown_busy);
        if(throws)throw std::runtime_error("callback");
        if(!cancel){++dispatched;texture[0]^=1;}
    }
};
__SCANNER__
int condvar_wait(KernelState&,Memory&,const char*,SceUID,int,SceUInt32*timeout,SyncWeight) {
    auto before=e::domain.stamp(ticket,0).ranges;
    // The actual scoped-tail call keeps timeout/workarea busy, even for zero callbacks.
    bool ok=e::declare_outputs({"sceKernelWaitLwCondCB","_sceKernelWaitLwCondCB"},{{timeout,sizeof(*timeout)}},true);
    auto after=e::domain.stamp(ticket,0).ranges;
    if(ok){assert(!after.unknown_busy);assert(bool(after.unknown!=before.unknown)==expected_unknown);assert(after.busy==uint64_t(expected_busy));}
    else assert(after.unknown_busy);
    return 17;
}
__WRAPPER__
struct Context {
    struct{int type=1;}state;
    uint64_t command_allocator_size=16;
    struct Pointer{uint32_t addr;void*p;uint32_t address()const{return addr;}void*get(const Memory&)const{return p;}};
    Pointer alloc_space{8192,nullptr},alloc_space_end{12288,nullptr};
};
namespace renderer{struct Command{uint8_t bytes[16];};}
constexpr int SCE_GXM_CONTEXT_TYPE_IMMEDIATE=1;
__STATE_SNIPPETS__
void setup(){assert(e::domain.reset());emuenv.kernel.thread=std::make_shared<Thread>();texture.fill(0);e::enabled=true;
    e::domain.watch_range(0,texture.data(),16,nullptr,0);ticket=e::domain.publish();expected_busy=0;}
void poll(bool recursive,bool cancel,bool fire,bool invalid=false){
    setup();if(invalid)emuenv.kernel.thread.reset();
    else {emuenv.kernel.thread->is_processing_callbacks=recursive;if(fire){auto cb=std::make_shared<Callback>();cb->cancel=cancel;emuenv.kernel.thread->callbacks.push_back(cb);}}
    SceKernelLwCondWork work;uint32_t timeout=8;expected_unknown=recursive||fire||invalid;
    e::domain.callback_audit().phase(1,true);
    auto before=e::domain.stamp(ticket,0).ranges;
    {e::HostLease root; e::name_import("sceKernelWaitLwCondCB");assert(_sceKernelWaitLwCondCB({&work},&timeout)==17);}
    auto after=e::domain.stamp(ticket,0).ranges;
    assert(e::stable_host(before,after)==!expected_unknown);
    auto stats=e::domain.callback_audit().snapshot();assert(stats.polls==1&&stats.uses==1);
    assert(stats.clean_uses==uint64_t(!expected_unknown));
    assert(stats.incomplete==uint64_t(recursive||invalid));
    e::domain.callback_audit().phase(99,false);
}
void negative_and_busy(){
    setup();SceKernelLwCondWork work;
    {e::HostLease root;e::name_import("sceKernelWaitLwCondCB");
     assert(e::declare_outputs({"sceKernelWaitLwCondCB"},{},true));}
    assert(e::domain.callback_audit().snapshot().conservative_uses==1); // missing scan cannot grant proof
    setup();expected_unknown=true;
    {e::HostLease root;e::name_import("unreviewed_outer");assert(_sceKernelWaitLwCondCB({&work},nullptr)==17);}
    setup();e::domain.clear_interests();e::domain.watch_range(0,texture.data(),4,nullptr,0);
    expected_unknown=false;expected_busy=1;
    {e::HostLease root;e::name_import("sceKernelWaitLwCondCB");assert(_sceKernelWaitLwCondCB({&work},reinterpret_cast<uint32_t*>(texture.data()))==17);}
    setup();auto cb=std::make_shared<Callback>();cb->throws=true;emuenv.kernel.thread->callbacks.push_back(cb);
    try{e::HostLease root;e::name_import("sceKernelWaitLwCondCB");_sceKernelWaitLwCondCB({&work},nullptr);assert(false);}catch(const std::runtime_error&){}
    assert(e::domain.callback_audit().snapshot().abandoned==1&&!e::domain.snapshot().host_active&&e::domain.snapshot().failed);
    setup();
    {e::HostLease root;e::name_import("sceKernelWaitLwCondCB");auto c=e::HostLease::callback_context();
     {e::CallbackPrefixScope first(c);first.finish(true,1);}
     {e::CallbackPrefixScope second(c);second.finish(true,0);assert(e::CallbackPrefixScope::prior_unknown(c,"sceKernelWaitLwCondCB"));}}
    setup();
    {e::HostLease outer;e::name_import("sceKernelWaitLwCondCB");auto c=e::HostLease::callback_context();e::CallbackPrefixScope p(c);p.finish(true,0);
     {e::HostLease inner;e::name_import("sceKernelWaitLwCondCB");assert(e::CallbackPrefixScope::prior_unknown(e::HostLease::callback_context(),"sceKernelWaitLwCondCB"));}
     assert(!e::CallbackPrefixScope::prior_unknown(c,"sceKernelWaitLwCondCB"));assert(e::CallbackPrefixScope::prior_unknown(c,"sceKernelWaitLwCondCB"));}
}
void contexts(){
    setup();Context context;context.alloc_space.p=texture.data()+128;
    __STATE_CALLS__
    // The same reviewed root under a deferred command path stays unknown.
    context.state.type=2;
    {e::HostLease h;e::name_import("sceGxmSetFrontVisibilityTestIndex");state_19(&context);assert(e::domain.stamp(ticket,0).ranges.unknown_busy);}
    context.state.type=1;e::domain.clear_interests();e::domain.watch_range(0,texture.data()+128,4,nullptr,0);
    {e::HostLease h;e::name_import("sceGxmSetFrontVisibilityTestIndex");state_19(&context);assert(e::domain.stamp(ticket,0).ranges.busy==1);}
}
void phases_and_concurrency(const char*path){
    setup();namespace w=switch_validation::watch;
    w::Table table;table.set_epoch_domain(&e::domain);auto raw=std::make_unique<w::Page[]>(w::page_count);assert(table.bind(raw.get()));
    table.phase(0,false,true);
    std::vector<std::thread>workers;
    for(unsigned k=0;k<4;++k)workers.emplace_back([]{for(unsigned i=0;i<1024;++i){
        e::HostLease root;e::name_import("sceKernelWaitLwCondCB");e::CallbackPrefixScope p(e::HostLease::callback_context());p.finish(true,0);
        assert(e::declare_outputs({"sceKernelWaitLwCondCB"},{},true));}});
    for(auto&t:workers)t.join();
    auto s=e::domain.callback_audit().snapshot();assert(s.polls==4096&&s.clean_uses==4096&&!e::domain.snapshot().failed);
    table.phase(1,false);{e::HostLease root;e::name_import("sceKernelWaitLwCondCB");e::CallbackPrefixScope p(e::HostLease::callback_context());p.finish(true,0);
        table.phase(2,false);assert(e::declare_outputs({"sceKernelWaitLwCondCB"},{},true));}
    table.phase(3,false);table.phase(w::no_index,false);table.detach(raw.get());
    FILE*f=std::fopen(path,"w");assert(f);e::domain.write_ranges(f);std::fclose(f);
}
int main(int argc,char**argv){assert(argc==2);poll(false,false,false);poll(false,false,true);poll(false,true,true);poll(true,false,false);poll(false,false,false,true);
    negative_and_busy();contexts();phases_and_concurrency(argv[1]);e::enabled=false;}
'''

def check_report(path):
    contracts=json.loads(path.read_text());r=contracts['callback_polls'];a=load(ROOT/'scripts/analyze-switch-epoch.py','callback_analyzer')
    issues=[];assert a.analyze_callback_polls({'range_contracts':contracts},issues)==r and not issues,issues
    assert r['phases'][0]['clean_uses']==4096 and r['phases'][1]['empty']==1 and r['phases'][2]['clean_uses']==1
    assert r['total']['polls']==4097 and r['total']['clean_uses']==4097 and r['active_phase']==4
    for field,value in [('version',True),('authority',True),('active_phase',1),('failed',True),('phases',[])]:
        bad=copy.deepcopy(contracts);bad['callback_polls'][field]=value;issues=[];a.analyze_callback_polls({'range_contracts':bad},issues);assert issues,field
    for field,value in [('polls',0),('reported_callbacks',-1),('uses',0),('clean_uses',4098),('abandoned',1)]:
        bad=copy.deepcopy(contracts);bad['callback_polls']['total'][field]=value;issues=[];a.analyze_callback_polls({'range_contracts':bad},issues);assert issues,field
    issues=[];assert a.analyze_callback_polls({},issues) is None and not issues
    for invalid in ([], True, 1, 'invalid'):
        issues=[];a.analyze_callback_polls({'range_contracts':{'callback_polls':invalid}},issues);assert issues


def main():
    p=argparse.ArgumentParser();p.add_argument('--cxx',default='c++');p.add_argument('--sanitize',action='store_true');p.add_argument('--require-source',action='store_true');args=p.parse_args()
    scanner,wrapper,states,calls=sources(args.require_source)
    code=CPP.replace('__SCANNER__',scanner).replace('__WRAPPER__',wrapper).replace('__STATE_SNIPPETS__',states).replace('__STATE_CALLS__',calls)
    with tempfile.TemporaryDirectory() as td:
        d=Path(td);(d/'test.cpp').write_text(code)
        cmd=[args.cxx,'-std=c++20','-O1'if args.sanitize else '-O2','-Wall','-Wextra','-Werror','-pthread','-D__SWITCH__','-DVITA3K_SWITCH_TRACELAB=1','-I',str(ROOT/'vita3k/util/include')]
        if args.sanitize:cmd+=['-fsanitize=address,undefined','-fno-omit-frame-pointer']
        subprocess.run(cmd+[str(d/'test.cpp'),'-o',str(d/'test')],check=True)
        subprocess.run([str(d/'test'),str(d/'report.json')],check=True,timeout=45);check_report(d/'report.json')
        # Real negative: dropping the poll evidence must fail the first empty-poll case.
        broken=code.replace('callback_prefix.finish(full_callback_scan, callbacks_processed);','(void)full_callback_scan; (void)callbacks_processed;')
        (d/'broken.cpp').write_text(broken);subprocess.run(cmd+[str(d/'broken.cpp'),'-o',str(d/'broken')],check=True)
        negative=subprocess.run([str(d/'broken'),str(d/'negative.json')],capture_output=True,timeout=45)
        assert negative.returncode!=0,'Missing callback proof incorrectly passed'
    print('Callback/state batch PASS: actual polling+wrapper bodies with explicit kernel adapters, empty/nonempty/cancelled/recursive/missing/throw/nested/sticky proofs, output busy, 23 context declarations, 4096 concurrent scans, production phase hooks+JSON/analyzer and missing-proof negative. Not native kernel/GPU execution.')
if __name__=='__main__':main()
