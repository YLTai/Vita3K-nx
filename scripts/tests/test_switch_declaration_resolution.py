#!/usr/bin/env python3
"""Parallel strict/resolved full-Hash evidence, using the production Table.
No Horizon, native JIT, GPU execution or reuse authority is implied.
"""
import argparse, copy, importlib.util, json, subprocess, tempfile
from pathlib import Path
ROOT = Path(__file__).resolve().parents[2]

def load(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module); return module

def source_gate(required):
    engine = load(ROOT / 'scripts/apply-switch-write-watch-hooks.py', 'resolution_engine')
    recipe = json.loads((ROOT / 'patches/experiments/switch-declaration-resolution.json').read_text())
    if required:
        chain = load(ROOT / 'scripts/tests/test_switch_controller_recipe_chain.py', 'resolution_chain')
        for item in recipe['files']:
            original, _, final = chain.replay(engine, item['path'], True)
            prior = original
            for rp in engine.RECIPES:
                if rp.name == 'switch-declaration-resolution.json': break
                for spec in json.loads(rp.read_text())['files']:
                    if spec['path'] == item['path']: prior = engine.transform(prior, spec['replacements'])
            own = engine.transform(prior, item['replacements'])
            assert own == final, item['path']
            text = own.decode()
            for op in reversed(item['replacements']):
                assert text.count(op['new']) == 1
                text = text.replace(op['new'], op['old'], 1)
            assert text.encode() == prior
        assert (ROOT / 'scripts/tests/run-switch-performance-tests.sh').read_text().splitlines().count(
            'python3 "$ROOT/scripts/tests/test_switch_declaration_resolution.py" --cxx "${CXX:-c++}" --require-source') == 1
    return recipe

CPP = r'''
#include <util/switch_write_watch.h>
#include <util/host_output_scope.h>
#include <util/switch_validation_status.h>
#include <cassert>
#include <array>
#include <string>
#include <thread>
#include <vector>
namespace e = switch_validation::epoch;
namespace w = switch_validation::watch;
using Block = e::ResolutionBlock;
uint64_t scheduled = 0;
__PUBLISH_HOOK__
void hud_controls(const w::Snapshot &observation) {
    using namespace switch_validation;
    auto out=publish_resolution(observation,true);
    assert(out.resolved_mismatches==1 && out.watch_failed && out.epoch_changes==1);
    out.epoch_audit=true;out.stage=Stage::Complete;out.outcome=CaptureOutcome::Invalid;
    auto hud=make_hud_text(out,0);
    assert(std::string(hud.title.data()).find("DONE - CHECK REPORT")!=std::string::npos);
    assert(std::string(hud.samples.data()).find("Resolved:")!=std::string::npos);
    assert(std::string(hud.samples.data()).find("Diff:1")!=std::string::npos);
    assert(std::string(hud.samples.data()).find("FULL Hash")!=std::string::npos);
    assert(std::string(hud.action.data()).find("EXIT GAME NORMALLY")!=std::string::npos);
    out.stage=Stage::Measuring;out.progress_per_mille=1000;out.remaining_ms=0;
    hud=make_hud_text(out,UINT64_MAX);
    assert(std::string(hud.title.data()).find("DONE")==std::string::npos);
    auto disabled=publish_resolution(observation,false);assert(!disabled.declaration_resolution&&!disabled.watch_failed);
}
__EXPORT_HOOK__
// Enumerate all order-preserving interleavings of four writer actions and four
// Hash endpoints. The writer's scope may begin before or finish after a Hash.
void interleaving(unsigned bits, unsigned kind) {
    e::HleRanges gate; std::array<uint8_t,64> bytes{};
    gate.watch(0,{bytes.data(),4},{bytes.data()+16,4});
    std::array<e::RangeStamp,4> points{};unsigned writer=0,point=0,slot=e::HleRanges::no_slot;
    uint8_t baseline=0,current=0;
    for(unsigned step=0;step<8;++step) {
        if(bits & (1u<<step)) {
            points[point]=gate.stamp(0);
            if(point==1) baseline=bytes[0]+bytes[16];
            if(point==3) current=bytes[0]+bytes[16];
            ++point;
        } else {
            if(writer==0) {slot=gate.enter(1);gate.name(slot,"operation");}
            if(writer==1 && kind!=0) {
                const auto offset=kind==1?32:kind==3?16:0;
                assert(gate.declare(slot,{"operation"},{{bytes.data()+offset,4}},kind==4));
            }
            if(writer==2 && kind!=1) bytes[kind==3?16:0]=1;
            if(writer==3) gate.leave(slot);
            ++writer;
        }
    }
    auto result=e::resolve_declarations(points[0],points[1],points[2],points[3]);
    const bool strict=e::stable_host(points[0],points[1])&&e::stable_host(points[1],points[2])&&e::stable_host(points[2],points[3]);
    if(strict) assert(result==Block::Accepted);
    // No correctly tracked writer can alter the content between comparable
    // baseline/current reads while still satisfying the resolved predicate.
    if(result==Block::Accepted) assert(baseline==current);
    assert(!gate.failed()); ++scheduled;
}
void predicate_controls() {
    e::RangeStamp a{1,0,0,0,0,true};auto b=a,c=a,d=a;
    assert(e::resolve_declarations(a,b,c,d)==Block::Accepted);
    for(unsigned mask=0;mask<8;++mask) {
        a.unknown_busy=mask&1;b.unknown_busy=(mask>>1)&1;c.unknown_busy=(mask>>2)&1;
        assert(e::resolve_declarations(a,b,c,d)==Block::Accepted);
    }
    d.unknown_busy=1;assert(e::resolve_declarations(a,b,c,d)==Block::Pending);d.unknown_busy=0;
    d.unknown=1;assert(e::resolve_declarations(a,b,c,d)==Block::History);d.unknown=0;
    d.version=2;assert(e::resolve_declarations(a,b,c,d)==Block::Version);d.version=1;
    for(auto *s:{&a,&b,&c,&d}) {
        s->busy=1;assert(e::resolve_declarations(a,b,c,d)==Block::OutputBusy);s->busy=0;
        s->valid=false;assert(e::resolve_declarations(a,b,c,d)==Block::Invalid);s->valid=true;
    }
    for(unsigned bits=0;bits<256;++bits) if(__builtin_popcount(bits)==4)
        for(unsigned kind=0;kind<5;++kind) interleaving(bits,kind);
    assert(scheduled==350);
}
void bounds_and_two_writers() {
    e::HleRanges gate;std::array<uint8_t,64> bytes{};
    gate.watch(0,{bytes.data(),4},{});const auto initial=gate.stamp(0);
    auto one=gate.enter(1),two=gate.enter(2);gate.name(one,"one");gate.name(two,"two");
    auto pending=gate.stamp(0);assert(pending.unknown_busy==2);
    assert(gate.declare(one,{"one"},{},false));auto partial=gate.stamp(0);
    assert(e::resolve_declarations(initial,pending,pending,partial)==Block::Pending);
    assert(gate.declare(two,{"two"},{{bytes.data()+32,4}},false));auto ready=gate.stamp(0);
    assert(e::resolve_declarations(initial,pending,partial,ready)==Block::Accepted);
    gate.leave(one);gate.leave(two);
    one=gate.enter(1);gate.name(one,"unknown");gate.leave(one);
    assert(e::resolve_declarations(initial,pending,partial,gate.stamp(0))==Block::History);
    e::ResolutionAudit bounded;
    auto a=initial,b=a,c=a,d=a;c.unknown_busy=1;
    for(unsigned n=0;n<40;++n)bounded.observe(1,0,8192,8,1,10,11,false,a,b,c,d);
    assert(bounded.stored==32 && bounded.dropped==8 && bounded.phases[1].additional_mismatches==40);
    e::ResolutionAudit overflow;
    overflow.observe(1,0,8192,UINT64_MAX,1,10,10,false,a,b,c,d);
    overflow.observe(1,0,8192,1,1,10,10,false,a,b,c,d);assert(overflow.failed);
}
struct Fixture {
    e::Domain domain;w::Table table;
    std::unique_ptr<uint8_t*[]> raw=std::make_unique<uint8_t*[]>(w::page_count);
    alignas(4096)std::array<uint8_t,8192> data{};
    Fixture(){table.set_epoch_domain(&domain);raw[2]=reinterpret_cast<uint8_t*>(uintptr_t(data.data())-8192);
        assert(table.bind(raw.get()));table.phase(0,false,true);hash();table.phase(1,true,false,true);hash();}
    uint64_t digest()const {uint64_t v=1469598103934665603ull;for(unsigned i:{0u,1u,2u,3u,16u,17u,18u,19u})v=(v^data[i])*1099511628211ull;return v;}
    w::Token begin(){return table.before(raw.get(),8192,data.data(),4,data.data()+16,4);}
    w::Audit finish(w::Token token){return table.after(token,digest());}
    w::Audit hash(){return finish(begin());}
    ~Fixture(){table.phase(w::no_index,false);table.detach(raw.get());}
};
void table_controls(const char *path) {
    Fixture f;
    f.hash(); // strict positive
    {
        e::HostLease h(&f.domain,1);e::name_import("disjoint");
        auto t=f.begin();assert(e::declare_outputs({"disjoint"},{{f.data.data()+32,4}}));f.finish(t);
    }
    auto s=f.table.snapshot();assert(s.declaration_resolution.phases[1].additional==1);
    assert(s.declaration_resolution.phases[1].additional_during_hash==1);
    // A pending import can span the whole prior Hash. Only its later reviewed
    // disjoint declaration, before the decision, resolves that old endpoint.
    {
        e::HostLease h(&f.domain,1);e::name_import("disjoint");f.hash();
        auto t=f.begin();assert(e::declare_outputs({"disjoint"},{{f.data.data()+32,4}}));f.finish(t);
    }
    assert(f.table.snapshot().declaration_resolution.phases[1].additional==2);
    { e::HostLease h(&f.domain,1);e::name_import("disjoint");f.hash();
      assert(e::declare_outputs({"disjoint"},{{f.data.data()+32,4}}));f.hash(); }
    assert(f.table.snapshot().declaration_resolution.phases[1].additional_start_idle==1);
    {
        e::HostLease h(&f.domain,1);e::name_import("unknown");auto t=f.begin();f.data[0]=4;f.finish(t);
    }
    f.hash(); // unknown history remains; not retroactively erased
    {
        auto t=f.begin();
        {e::HostLease h(&f.domain,1);e::name_import("overlap");assert(e::declare_outputs({"overlap"},{{f.data.data()+16,4}}));}
        f.finish(t); // same-value palette output still invalidates
    }
    f.hash();
    {
        auto t=f.begin();{e::HostLease h(&f.domain,1);e::name_import("callback");assert(e::declare_outputs({"callback"},{},true));}
        f.finish(t);
    }
    f.hash();
    {
        auto t=f.begin();f.domain.gpu_boundary();f.finish(t);
    }
    // Deliberately violate a declared empty-output contract. The real reference
    // Hash must contradict the recovered candidate; no game data is involved.
    f.hash();
    {e::HostLease h(&f.domain,1);e::name_import("broken");auto t=f.begin();
     assert(e::declare_outputs({"broken"},{}));f.data[0]^=0x80;f.finish(t);}
    s=f.table.snapshot();const auto &r=s.declaration_resolution.phases[1];
    assert(!s.declaration_resolution.failed && r.mismatches==1 && r.additional_mismatches==1);
    assert(r.pending && r.history && r.version && r.gpu_history);
    assert(r.checked==s.phases[1].cpu_candidates&&r.strict==s.phases[1].hle_candidates);
    assert(r.resolved==r.strict+r.additional&&r.submission+r.gpu_history==r.resolved);
    assert(s.declaration_resolution.stored==1);
    hud_controls(s);
    FILE *out=std::fopen(path,"w");assert(out);s.declaration_resolution.write_json(out);std::fclose(out);
    const auto document=std::string(path)+".document";
    out=std::fopen(document.c_str(),"w");assert(out);export_resolution(out,f.table,true);std::fclose(out);
    auto token=f.begin();f.table.phase(0,false,true);f.finish(token);
    for(const auto &c:f.table.snapshot().declaration_resolution.phases)assert(!c.checked);
    // Mapping lifetime change must prevent scope construction/candidate reuse.
    f.hash();f.table.phase(1,true,false,true);f.hash();token=f.begin();
    f.table.publish(f.raw.get(),2,nullptr);auto audit=f.finish(token);assert(audit.reason==w::HashReason::Raced);
    assert(!f.table.snapshot().declaration_resolution.phases[1].checked);
}
void saturation_and_concurrency() {
    e::Domain domain;alignas(4096)std::array<uint8_t,4096> data{};
    domain.watch_range(0,data.data(),4,nullptr,0);const auto ticket=domain.publish();
    std::vector<std::thread> threads;
    for(unsigned t=0;t<4;++t)threads.emplace_back([&]{for(unsigned n=0;n<1024;++n){
        e::HostLease h(&domain,1);e::name_import("read");assert(e::declare_outputs({"read"},{}));}});
    for(auto &t:threads) { t.join(); }
    assert(!domain.snapshot().failed&&domain.snapshot().host_entries==4096);
    auto a=domain.stamp(ticket,0).ranges;assert(e::resolve_declarations(a,a,a,a)==Block::Accepted);
    std::array<unsigned,e::HleRanges::max_imports> slots{};
    for(auto &s:slots)s=domain.host_enter(1);
    const auto extra=domain.host_enter(1);assert(extra==e::HleRanges::no_slot);
    auto invalid=domain.stamp(ticket,0).ranges;assert(e::resolve_declarations(a,a,a,invalid)==Block::Invalid);
}
int main(int argc,char **argv){assert(argc==2);predicate_controls();bounds_and_two_writers();table_controls(argv[1]);saturation_and_concurrency();}
'''

def analyzer_controls(report, analyzer):
    counts = report['phases']
    data = dict(declaration_resolution_expected=True, declaration_resolution=report,
                write_shadow={'resources':1}, epoch_audit={
                    'phases':[{'cpu_candidates':r['checked']} for r in counts],
                    'range_phases':[{'hle_candidates':r['strict'],'hle_candidate_changes':r['mismatches']-r['additional_mismatches']} for r in counts],
                    'candidate_work':{'phases':[{'cpu_bytes':r['checked_bytes'],'hle_bytes':r['strict_bytes']} for r in counts]}})
    issues=[];warnings=[];result=analyzer.analyze_declaration_resolution(data,issues,warnings)
    assert not issues and warnings and result['totals']['additional_mismatches']==1 and not result['safe_to_skip_hash'],issues
    assert analyzer.analyze_declaration_resolution({},[],[]) is None
    for key,value in [('version',True),('version',2),('authority',True),('full_hash_each_check',False),('GPU_completion_covered',True),('async_HLE_covered',True),('failed',True),('rule','relaxed'),('phases',[])]:
        bad=copy.deepcopy(data);bad['declaration_resolution'][key]=value;issues=[]
        analyzer.analyze_declaration_resolution(bad,issues,[]);assert issues,(key,value)
    for key in counts[1]:
        bad=copy.deepcopy(data);bad['declaration_resolution']['phases'][1][key]+=1;issues=[]
        analyzer.analyze_declaration_resolution(bad,issues,[]);assert issues,key
    for action in ('missing','marker','event','final_pending','equal','old_ledger'):
        bad=copy.deepcopy(data)
        if action=='missing':del bad['declaration_resolution']
        elif action=='marker':del bad['declaration_resolution_expected']
        elif action=='event':bad['declaration_resolution']['events']=[]
        elif action=='final_pending':bad['declaration_resolution']['events'][0]['pending'][-1]=1
        elif action=='equal':bad['declaration_resolution']['events'][0]['current_hash_hex']=bad['declaration_resolution']['events'][0]['previous_hash_hex']
        else:bad['epoch_audit']['phases'][1]['cpu_candidates']+=1
        issues=[];analyzer.analyze_declaration_resolution(bad,issues,[]);assert issues,action


def main():
    p=argparse.ArgumentParser();p.add_argument('--cxx',default='c++');p.add_argument('--sanitize',action='store_true');p.add_argument('--require-source',action='store_true');args=p.parse_args()
    recipe=source_gate(args.require_source)
    with tempfile.TemporaryDirectory() as name:
        d=Path(name);cpp=d/'test.cpp'
        controller=next(x for x in recipe['files'] if x['path'].endswith('switch_memory_validation.cpp'))['replacements']
        publish='switch_validation::StatusSnapshot publish_resolution(const w::Snapshot &observation,bool active) {\n'
        publish+='switch_validation::StatusSnapshot out;struct {bool epoch_audit;}options{active};\n'+controller[0]['new']+'\nreturn out;\n}'
        export='void export_resolution(FILE *file,w::Table &live_watch,bool active){\n'
        export+='struct {bool epoch_audit;}options{active};std::fputs("{\\\"sentinel\\\":42",file);\n'
        export+=controller[1]['new']+'\nassert(!write_error);\n}'
        cpp.write_text(CPP.replace('__PUBLISH_HOOK__',publish).replace('__EXPORT_HOOK__',export))
        cmd=[args.cxx,'-std=c++20','-O2','-Wall','-Wextra','-Werror','-pthread','-I',str(ROOT/'vita3k/util/include')]
        if args.sanitize:cmd+=['-fsanitize=address,undefined','-fno-omit-frame-pointer']
        subprocess.run(cmd+[str(cpp),'-o',str(d/'test')],check=True)
        subprocess.run([str(d/'test'),str(d/'report.json')],check=True,timeout=40)
        analyzer=load(ROOT/'scripts/analyze-switch-epoch.py','resolution_analyzer')
        report=json.loads((d/'report.json').read_text());analyzer_controls(report,analyzer)
        document=json.loads((d/'report.json.document').read_text())
        assert document==dict(sentinel=42,declaration_resolution_expected=True,declaration_resolution=report)
        # Faulty predicate: accepting an unresolved final endpoint must fail.
        inc=d/'mutant/util';inc.mkdir(parents=True)
        header=(ROOT/'vita3k/util/include/util/declaration_resolution.h').read_text()
        old='    if (d.unknown_busy) return ResolutionBlock::Pending;'
        assert header.count(old)==1;(inc/'declaration_resolution.h').write_text(header.replace(old,''))
        subprocess.run(cmd[:1]+['-I',str(inc.parent)]+cmd[1:]+[str(cpp),'-o',str(d/'mutant-test')],check=True)
        negative=subprocess.run([str(d/'mutant-test'),str(d/'bad.json')],capture_output=True,timeout=40)
        assert negative.returncode!=0 and b'Block::Pending' in negative.stderr,negative.stderr
    print('Declaration resolution PASS: 350 ordered writer/Hash schedules, real Table parallel old/new oracle, final-pending/overlap/palette/history/callback/GPU-boundary exclusions, map/restart, 4096 concurrent leases, actual JSON and corrupt-report negatives. No native/GPU or Hash-skip authority.')
if __name__=='__main__':main()
