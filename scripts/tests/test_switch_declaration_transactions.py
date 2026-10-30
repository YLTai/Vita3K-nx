#!/usr/bin/env python3
"""Execute accepted-declaration transactions and the real patched hook blocks.
Kernel/Ptr adapters are explicit; --require-source replays whole pinned files.
This is not Horizon execution, a GPU test, or permission to skip Hash.
"""
import argparse, copy, importlib.util, json, subprocess, tempfile
from pathlib import Path
ROOT = Path(__file__).resolve().parents[2]

def load(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module); return module

def source_gate(required):
    engine = load(ROOT / 'scripts/apply-switch-write-watch-hooks.py', 'transaction_engine')
    recipe = json.loads((ROOT / 'patches/experiments/switch-declaration-transactions.json').read_text())
    if required:
        chain = load(ROOT / 'scripts/tests/test_switch_controller_recipe_chain.py', 'transaction_chain')
        for item in recipe['files']:
            before, _, after = chain.replay(engine, item['path'], True)
            for op in item['replacements']:
                assert op['new'] in after.decode(), item['path']
            # Removing our additions must recover the complete prior expansion.
            reverted = after.decode()
            for op in reversed(item['replacements']):
                assert reverted.count(op['new']) == 1
                reverted = reverted.replace(op['new'], op['old'], 1)
            prior = before
            for rp in engine.RECIPES:
                if rp.name == 'switch-declaration-transactions.json': break
                for spec in json.loads(rp.read_text())['files']:
                    if spec['path'] == item['path']: prior = engine.transform(prior, spec['replacements'])
            assert reverted.encode() == prior
        for path, pin in recipe['reviewed_dependencies'].items():
            original, _, _ = chain.replay(engine, path, True)
            assert engine.blob_sha(original) == pin
        driver = (ROOT / 'scripts/tests/run-switch-performance-tests.sh').read_text()
        assert driver.splitlines().count('python3 "$ROOT/scripts/tests/test_switch_declaration_transactions.py" --cxx "${CXX:-c++}" --require-source') == 1
    return recipe

EXTRA = r'''
#include <map>
#include <util/host_output_scope.h>
void transactions() {
    setup();
    { e::HostLease root; e::name_import("sceKernelWaitLwCondCB");
      const auto c=e::HostLease::callback_context(); e::CallbackPrefixScope p(c); p.finish(true,0);
      assert(!e::declare_outputs({"sceKernelLockLwMutexCB"},{},true));
      assert(e::domain.callback_audit().snapshot().uses == 0);
      e::HostSpan bad; bad.begin=UINTPTR_MAX-1; bad.size=8;
      assert(!e::declare_outputs({"sceKernelWaitLwCondCB"},{bad},true));
      assert(!e::declare_outputs({"sceKernelWaitLwCondCB"},{ {},{},{},{} },true));
      assert(e::domain.callback_audit().snapshot().uses == 0 && !*c.tainted);
      assert(e::declare_outputs({"sceKernelWaitLwCondCB"},{{texture.data(),4}},true));
      assert(e::domain.stamp(ticket,0).ranges.busy==1);
      assert(!e::declare_outputs({"sceKernelWaitLwCondCB"},{},true));
      assert(!e::declare_outputs({"sceKernelLockLwMutexCB"},{},true));
      auto counts=e::domain.callback_audit().snapshot();
      assert(counts.uses==1 && counts.clean_uses==1 && counts.conservative_uses==0 && !*c.tainted);
    }
    assert(!e::domain.stamp(ticket,0).ranges.busy);
    setup();
    { e::HostLease root; e::name_import("sceKernelWaitLwCondCB");
      e::CallbackPrefixScope p(e::HostLease::callback_context()); p.finish(true,1);
      assert(!e::declare_outputs({"sceKernelLockLwMutexCB"},{},true));
      assert(e::declare_outputs({"sceKernelWaitLwCondCB"},{},true));
      assert(e::domain.callback_audit().snapshot().conservative_uses==1);
    }
}
struct Program {uint32_t reference_count=1;};
struct ProgramPtr {Program *p=nullptr; Program *get(Memory&) const{return p;}};
struct Patcher {std::map<int,ProgramPtr> fragment_program_cache,vertex_program_cache;};
__HOOKS__
void branches() {
    setup(); Context context; ProgramPtr out; Program program; Patcher patcher;
    patcher.fragment_program_cache[1]={&program};patcher.vertex_program_cache[1]={&program};
    e::domain.watch_range(1,&context,sizeof(context),nullptr,0);
    e::domain.watch_range(2,&out,sizeof(out),nullptr,0);
    e::domain.watch_range(3,&program.reference_count,sizeof(program.reference_count),nullptr,0);
    for (auto entry : {std::make_pair("sceGxmReserveFragmentDefaultUniformBuffer", &reserve_fragment),
                       std::make_pair("sceGxmReserveVertexDefaultUniformBuffer", &reserve_vertex)}) {
        { e::HostLease root;e::name_import(entry.first);entry.second(&context,&out);
          assert(e::domain.stamp(ticket,1).ranges.busy==1 && e::domain.stamp(ticket,2).ranges.busy==1);
          assert(!e::domain.stamp(ticket,0).ranges.busy && !e::domain.stamp(ticket,0).ranges.unknown_busy); }
        context.state.type=2;
        { e::HostLease root;e::name_import(entry.first);entry.second(&context,&out);
          assert(e::domain.stamp(ticket,0).ranges.unknown_busy); }
        context.state.type=1;
        { e::HostLease root;e::name_import("unreviewed_outer");entry.second(&context,&out);
          assert(e::domain.stamp(ticket,0).ranges.unknown_busy); }
        { e::HostLease root;e::name_import(entry.first);entry.second(nullptr,&out);
          assert(e::domain.stamp(ticket,0).ranges.unknown_busy); }
    }
    for (auto entry : {std::make_pair("sceGxmShaderPatcherCreateFragmentProgram", &cache_fragment),
                       std::make_pair("sceGxmShaderPatcherCreateVertexProgram", &cache_vertex)}) {
        auto prior=e::domain.stamp(ticket,2).ranges;
        { e::HostLease root;e::name_import(entry.first);
          assert(entry.second(&patcher,&out,true)==0);
          assert(e::domain.stamp(ticket,2).ranges.busy==1 && e::domain.stamp(ticket,3).ranges.busy==1); }
        assert(out.p==&program && !e::stable_host(prior,e::domain.stamp(ticket,2).ranges));
        { e::HostLease root;e::name_import(entry.first);
          assert(entry.second(&patcher,&out,false)==-1);
          assert(e::domain.stamp(ticket,0).ranges.unknown_busy); }
        { e::HostLease root;e::name_import("unreviewed_outer");assert(entry.second(&patcher,&out,true)==0);
          assert(e::domain.stamp(ticket,0).ranges.unknown_busy); }
    }
    // The signalling root may be host-only; another writer's busy output is NOT waived.
    for (auto name : {"sceKernelSignalLwCond","sceKernelSignalLwCondAll","sceKernelSignalLwCondTo","sceKernelSignalSema"}) {
        e::HostOutputLease writer(texture.data(),{4,true});
        e::HostLease signal;e::name_import(name);
        if(std::string(name)=="sceKernelSignalSema")signal_sema();else signal_cond();
        assert(e::domain.stamp(ticket,0).ranges.busy==1 && !e::domain.stamp(ticket,0).ranges.unknown_busy);
        {e::NativeWaitLease native_wait;assert(e::domain.stamp(ticket,0).ranges.busy==1);}
    }
    { e::HostLease root;e::name_import("sceKernelSetEventFlag");signal_cond();signal_sema();
      assert(e::domain.stamp(ticket,0).ranges.unknown_busy); }
}
void phase_boundaries(const char *path) {
    setup();
    // An import starting before the capture ends during phase zero.
    {e::HostLease old;e::name_import("old_unknown");e::domain.range_phase(0,true);e::domain.callback_audit().phase(0,true);}
    {e::HostLease root;e::name_import("read");e::declare_outputs({"read"},{});
     e::domain.range_phase(1,false);e::domain.callback_audit().phase(1,false);}
    // Restart must not drop an output which is already active.
    {e::HostOutputLease output(texture.data(),{4,true});
     e::domain.range_phase(0,true);e::domain.callback_audit().phase(0,true);
     assert(e::domain.stamp(ticket,0).ranges.busy==1);}
    {e::HostLease unknown;e::name_import("phase_unknown");}
    e::domain.range_phase(4,false);e::domain.callback_audit().phase(4,false);
    FILE *f=std::fopen(path,"w");assert(f);e::domain.write_ranges(f);std::fclose(f);
}
'''

def hook_code(recipe):
    hooks=[]
    for i,op in enumerate(recipe['files'][0]['replacements']):
        snippet=op['new'][len(op['old']):]
        hooks.append('void '+('signal_sema' if i==0 else 'signal_cond')+'(){'+snippet+'\n}')
    for i,op in enumerate(recipe['files'][1]['replacements'][:2]):
        snippet=op['new'][len(op['old']):]
        hooks.append('void reserve_'+('fragment' if i==0 else 'vertex')+'(Context*context,ProgramPtr*uniformBuffer){'+snippet+'\n}')
    for i,op in enumerate(recipe['files'][1]['replacements'][2:]):
        part='fragment' if i==0 else 'vertex'
        hooks.append(f'int cache_{part}(Patcher*shaderPatcher,ProgramPtr*{part}Program,bool hit){{\n'
            'auto &mem=emuenv.mem;\n'+f'auto cached=hit?shaderPatcher->{part}_program_cache.begin():shaderPatcher->{part}_program_cache.end();\n'+op['new']+'\nreturn -1;\n}')
    return '\n'.join(hooks)

def validate_report(path, analyzer):
    c=json.loads(path.read_text());e={'range_contracts':c};issues=[]
    out=analyzer.analyze_phase_attribution(e,issues);assert out and not issues,(issues,out)
    assert out['phases'][0]['total']['enters']==4096
    assert out['phases'][1]['total']['enters']==1 and out['phases'][1]['total']['exits']==0
    assert out['phases'][2]['total']['declarations']==1 and out['phases'][2]['total']['exits']==1
    for field,value in [('version',True),('authority',True),('active_phase',1),('capture_serial',77),('phases',[])]:
        bad=copy.deepcopy(e);bad['range_contracts']['phase_attribution'][field]=value;issues=[]
        analyzer.analyze_phase_attribution(bad,issues);assert issues,field
    for mode in ('sum','duplicate','unknown','negative','prefix'):
        bad=copy.deepcopy(e);p=bad['range_contracts']['phase_attribution']['phases'][0]
        if mode=='sum':p['total']['enters']+=1
        if mode=='duplicate':p['imports'].append(p['imports'][0])
        if mode=='unknown':p['imports'][0]['nid']=0xffffffff
        if mode=='negative':p['unattributed']['enters']=-1
        if mode=='prefix':p['imports'][0]['counts']['unknown_prefixes']=5000
        issues=[];analyzer.analyze_phase_attribution(bad,issues);assert issues,mode
    issues=[];assert analyzer.analyze_phase_attribution({},issues) is None and not issues

def main():
    p=argparse.ArgumentParser();p.add_argument('--cxx',default='c++');p.add_argument('--sanitize',action='store_true');p.add_argument('--require-source',action='store_true');a=p.parse_args()
    recipe=source_gate(a.require_source)
    prior=load(ROOT/'scripts/tests/test_switch_callback_batch.py','transaction_callback')
    scanner,wrapper,states,calls=prior.sources(a.require_source)
    code=prior.CPP.replace('__SCANNER__',scanner).replace('__WRAPPER__',wrapper).replace('__STATE_SNIPPETS__',states).replace('__STATE_CALLS__',calls)
    extra=EXTRA.replace('__HOOKS__',hook_code(recipe))
    code=code.replace('int main(int argc,char**argv)',extra+'\nint main(int argc,char**argv)')
    code=code.replace('negative_and_busy();contexts();phases_and_concurrency(argv[1]);','negative_and_busy();transactions();branches();phase_boundaries("restart.json");contexts();phases_and_concurrency(argv[1]);')
    # Execute the same nested callback-mutex declaration names used by real condvar_wait.
    scoped=json.loads((ROOT/'patches/experiments/switch-scoped-hle.json').read_text())
    sync=next(x for x in scoped['files'] if x['path'].endswith('sync_primitives.cpp'))
    lock=next(x['new'] for x in sync['replacements'] if x['old'].startswith('inline static int mutex_lock_impl'))
    names=lock.split('declare_outputs({')[2].split('}')[0]
    assert names=='"sceKernelLockLwMutexCB","sceKernelLockMutexCB","_sceKernelLockMutexCB"'
    code=code.replace('    return 17;','    assert(!e::declare_outputs({'+names+'},{{timeout,sizeof(*timeout)}},true));\n    return 17;')
    with tempfile.TemporaryDirectory() as tmp:
        d=Path(tmp);(d/'test.cpp').write_text(code)
        cmd=[a.cxx,'-std=c++20','-O1' if a.sanitize else '-O2','-Wall','-Wextra','-Werror','-pthread','-D__SWITCH__','-DVITA3K_SWITCH_TRACELAB=1','-I',str(ROOT/'vita3k/util/include')]
        if a.sanitize:cmd+=['-fsanitize=address,undefined','-fno-omit-frame-pointer']
        subprocess.run(cmd+[str(d/'test.cpp'),'-o',str(d/'test')],check=True)
        subprocess.run([str(d/'test'),str(d/'ranges.json')],cwd=d,check=True,timeout=45)
        analyzer=load(ROOT/'scripts/analyze-switch-epoch.py','transaction_analyzer')
        validate_report(d/'ranges.json',analyzer)
        issues=[];reset=json.loads((d/'restart.json').read_text());analyzer.analyze_phase_attribution({'range_contracts':reset},issues);assert not issues,issues
        assert reset['phase_attribution']['capture_serial']==2 and reset['phase_attribution']['phases'][0]['total']['enters']==1
        # Negative control: preserve every test/header, only restore consume-before-admit.
        if not a.sanitize:
            shadow=d/'include/util';shadow.mkdir(parents=True)
            header=(ROOT/'vita3k/util/include/util/jit_epoch_gate.h').read_text()
            gate='        if (!ranges_.can_declare(slot, names, spans)) return false;\n'
            assert header.count(gate)==1
            header=header.replace(gate,'').replace('        return ranges_.declare(slot, names, spans, prior_unknown);',gate+'        return ranges_.declare(slot, names, spans, prior_unknown);')
            (shadow/'jit_epoch_gate.h').write_text(header)
            badcmd=cmd[:];index=badcmd.index('-I');badcmd[index:index]=['-I',str(d/'include')]
            subprocess.run(badcmd+[str(d/'test.cpp'),'-o',str(d/'bad')],check=True)
            result=subprocess.run([str(d/'bad'),str(d/'bad.json')],cwd=d,capture_output=True)
            assert result.returncode and b'stats.polls==1&&stats.uses==1' in result.stderr,result.stderr
    print('Declaration transactions PASS: real hook blocks, nested condvar/mutex sequence, rejected/invalid/repeated admission, actual-callback conservation, signal-other-writer, immediate/deferred reserve, cached/miss/unknown program paths, 4096 concurrent roots, per-phase/start-restart/export JSON and rejection tests. Restored old ordering fails. Host adapters, not Horizon/GPU execution.')

if __name__=='__main__':main()
