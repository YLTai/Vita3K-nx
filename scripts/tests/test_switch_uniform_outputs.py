#!/usr/bin/env python3
"""Concrete uniform write sites and read-only leaf regression.

Default: reviewed function fixture with explicit parameter/F16 host adapters.
--require-source: replay WHOLE pinned files and execute actual expanded bodies.
Neither mode claims target ABI, native shader/GPU execution or Hash authority.
"""
import argparse
import copy
import importlib.util
import json
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
RECIPE = ROOT / 'patches/experiments/switch-uniform-outputs.json'
LEAVES = {
    'sceClibMemcmp': ('int', 'const void *s1, const void *s2, SceSize len', 's1, s2, len', 'return memcmp(s1, s2, len);'),
    'sceClibStrcmp': ('int', 'const char *s1, const char *s2', 's1, s2', 'return strcmp(s1, s2);'),
    'sceClibStrncmp': ('int', 'const char *s1, const char *s2, SceSize len', 's1, s2, len', 'return strncmp(s1, s2, len);'),
    'sceClibStrncasecmp': ('int', 'const char *s1, const char *s2, SceSize len', 's1, s2, len', '#ifdef _WIN32\n    return _strnicmp(s1, s2, len);\n#else\n    return strncasecmp(s1, s2, len);\n#endif'),
    'sceClibStrnlen': ('uint32_t', 'const char *s1, SceSize maxlen', 's1, maxlen', 'return static_cast<uint32_t>(strnlen(s1, maxlen));'),
}


def load(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def body(text, signature):
    begin = text.index(signature)
    pos = text.index('{', begin) + 1
    depth = 1
    while depth:
        depth += (text[pos] == '{') - (text[pos] == '}')
        pos += 1
    return text[begin:pos]


def sources(required):
    recipe = json.loads(RECIPE.read_text())
    engine = load(ROOT / 'scripts/apply-switch-write-watch-hooks.py', 'uniform_engine')
    chain = load(ROOT / 'scripts/tests/test_switch_controller_recipe_chain.py', 'uniform_chain')
    originals, expanded = {}, {}
    if required:
        for item in recipe['files']:
            before, _, after = chain.replay(engine, item['path'], True)
            originals[item['path']] = before.decode()
            expanded[item['path']] = after.decode()
        for path, pin in recipe['reviewed_dependencies'].items():
            assert engine.blob_sha((ROOT / path).read_bytes()) == pin, path
        driver = expanded[recipe['files'][2]['path']]
        command = 'python3 "$ROOT/scripts/tests/test_switch_uniform_outputs.py" --cxx "${CXX:-c++}" --require-source'
        assert driver.splitlines().count(command) == 1
    else:
        print('Sparse fixture mode: whole-file production integration is NOT asserted.')
    gxm = recipe['files'][0]
    original = originals.get(gxm['path'], (ROOT / 'scripts/tests/fixtures/switch_uniform_original.cpp').read_text())
    convert = body(original, 'template <typename T>\nstatic void convert_uniform_data(')
    original = body(original, 'EXPORT(int, sceGxmSetUniformDataF,')
    # Every new production change must be an insertion, never a layout rewrite.
    operations = gxm['replacements'][1:]
    for item in operations:
        assert item['old'] in item['new']
    changed = engine.transform(original.encode(), operations).decode()
    if required:
        assert changed == body(expanded[gxm['path']], 'EXPORT(int, sceGxmSetUniformDataF,')
        assert convert == body(expanded[gxm['path']], 'template <typename T>\nstatic void convert_uniform_data(')
    leaf_before, leaf_after = [], []
    lib = recipe['files'][1]
    for name, (ret, args, trace, expr) in LEAVES.items():
        text = f'EXPORT({ret}, {name}, {args}) {{\n    TRACY_FUNC({name}, {trace});\n    {expr}\n}}'
        if required:
            text = body(originals[lib['path']], f'EXPORT({ret}, {name},')
        op = next(op for op in lib['replacements'] if f'TRACY_FUNC({name},' in op['old'])
        new = engine.transform(text.encode(), [op]).decode()
        if required:
            assert new == body(expanded[lib['path']], f'EXPORT({ret}, {name},')
        leaf_before.append(text)
        leaf_after.append(new)
    return convert, original, changed, '\n'.join(leaf_before), '\n'.join(leaf_after), operations


CPP = r'''
#include <util/host_output_scope.h>
#include <algorithm>
#include <array>
#include <cassert>
#include <cstring>
#include <strings.h>
#include <vector>
#include <stdexcept>
namespace e = switch_validation::epoch;
using SceSize = uint32_t;
enum SceGxmParameterType { SCE_GXM_PARAMETER_TYPE_S8, SCE_GXM_PARAMETER_TYPE_U8,
 SCE_GXM_PARAMETER_TYPE_U16, SCE_GXM_PARAMETER_TYPE_S16, SCE_GXM_PARAMETER_TYPE_U32,
 SCE_GXM_PARAMETER_TYPE_S32, SCE_GXM_PARAMETER_TYPE_F16, SCE_GXM_PARAMETER_TYPE_F32 };
enum SceGxmParameterCategory { SCE_GXM_PARAMETER_CATEGORY_UNIFORM, other_category };
// Explicit host adapter, not a target binary layout declaration.
struct SceGxmProgramParameter { SceGxmParameterCategory category; SceGxmParameterType type;
 uint32_t array_size, component_count, resource_index; };
namespace gxp {
int get_parameter_type_size(SceGxmParameterType t) {
 switch(t) {
 case SCE_GXM_PARAMETER_TYPE_S8: case SCE_GXM_PARAMETER_TYPE_U8: return 1;
 case SCE_GXM_PARAMETER_TYPE_F16: case SCE_GXM_PARAMETER_TYPE_U16: case SCE_GXM_PARAMETER_TYPE_S16: return 2;
 default: return 4;
 }
}
}
// Deliberate deterministic F16 adapter. This suite tests copy addresses/leases,
// not floating-point conversion accuracy. Both old/new bodies use this adapter.
void float_to_half(const float *src, uint16_t *dst, int count) {
 for(int i=0;i<count;++i) dst[i]=static_cast<uint16_t>(src[i]);
}
constexpr int SCE_GXM_ERROR_INVALID_POINTER=-1, SCE_GXM_ERROR_INVALID_VALUE=-2;
#define EXPORT(ret,name,...) ret name(__VA_ARGS__)
#define TRACY_FUNC(...) ((void)0)
#define RET_ERROR(e) (e)
using Copy = std::pair<size_t,size_t>;
std::array<uint8_t,512> reference_bytes, observed_bytes;
std::vector<Copy> copies;
uint8_t *current_base=nullptr;
bool checking=false, late=false, throw_copy=false;
uint64_t ticket=0, cases=0;
void checked_copy(void *destination, const void *source, size_t count) {
 auto *dst=static_cast<uint8_t*>(destination);
 assert(dst>=current_base&&dst+count<=current_base+512);
 copies.emplace_back(size_t(dst-current_base),count);
 if(checking&&e::enabled) {
  assert(e::domain.output_stats().active==(count?1u:0u));
  for(unsigned i=0;i<512;++i) {
   const bool overlap=observed_bytes.data()+i>=dst&&observed_bytes.data()+i<dst+count;
   assert(e::domain.stamp(ticket,i).ranges.busy==unsigned(overlap));
  }
  if(late&&count) { // new palette interest must immediately inherit an active output
   e::domain.watch_range(512,observed_bytes.data()+511,1,dst+count-1,1);
   assert(e::domain.stamp(ticket,512).ranges.busy==1);late=false;
  }
 }
 if(throw_copy) throw std::runtime_error("modeled copy failure");
 std::memcpy(destination,source,count);
}
namespace reference {
__CONVERT__
__ORIGINAL__
__LEAF_BEFORE__
}
namespace observed {
__CONVERT__
__CHANGED__
__LEAF_AFTER__
}
void prepare() {
 assert(e::domain.reset());
 for(unsigned i=0;i<512;++i) e::domain.watch_range(i,observed_bytes.data()+i,1,nullptr,0);
 ticket=e::domain.publish();
}
void compare(SceGxmParameterType type, unsigned components, unsigned arrays, unsigned offset, unsigned count) {
 prepare();reference_bytes.fill(0x5a);observed_bytes=reference_bytes;
 std::array<float,512> source;for(unsigned i=0;i<source.size();++i)source[i]=float(i%31+1);
 SceGxmProgramParameter p{SCE_GXM_PARAMETER_CATEGORY_UNIFORM,type,arrays,components,4};
 copies.clear();current_base=reference_bytes.data();checking=false;
 assert(reference::sceGxmSetUniformDataF(current_base,&p,offset,count,source.data())==0);
 auto expected=copies;copies.clear();current_base=observed_bytes.data();checking=true;
 const auto before=e::domain.stamp(ticket,16).ranges;
 { e::HostLease root; e::name_import("sceGxmSetUniformDataF");
  assert(observed::sceGxmSetUniformDataF(current_base,&p,offset,count,source.data())==0);
  assert(!e::domain.stamp(ticket,16).ranges.unknown_busy);
 }
 assert(copies==expected&&reference_bytes==observed_bytes);
 auto st=e::domain.output_stats();uint64_t bytes=0,starts=0;
 for(auto [at,n]:expected){(void)at;bytes+=n;starts+=n!=0;}
 assert(st.begins==starts&&st.ends==starts&&!st.active&&st.capacity_bytes==bytes&&st.sources[4]==starts);
 assert(!e::domain.snapshot().failed);
 // Repeat equal data: output versions must still move wherever stores occur.
 std::array<e::RangeStamp,512> stamps;
 for(unsigned i=0;i<512;++i)stamps[i]=e::domain.stamp(ticket,i).ranges;
 {e::HostLease root;e::name_import("sceGxmSetUniformDataF");
  observed::sceGxmSetUniformDataF(current_base,&p,offset,count,source.data());}
 for(unsigned i=0;i<512;++i) {
  bool touched=false;for(auto [at,n]:expected)if(i>=at&&i<at+n)touched=true;
  auto after=e::domain.stamp(ticket,i).ranges;
  assert(!after.busy&&e::stable_host(stamps[i],after)==!touched);
 }
 assert(e::domain.stamp(ticket,16).ranges.unknown==before.unknown);
 ++cases;
}
void leaves() {
 prepare();char a[]="Alpha",b[]="alpha";auto before=e::domain.stamp(ticket,0).ranges;
 #define LEAF(name, ...) { e::HostLease h;e::name_import(#name); \
  assert(observed::name(__VA_ARGS__)==reference::name(__VA_ARGS__)); \
  assert(!e::domain.stamp(ticket,0).ranges.unknown_busy); }
 LEAF(sceClibMemcmp,a,b,5);LEAF(sceClibStrcmp,a,b);LEAF(sceClibStrncmp,a,b,3);
 LEAF(sceClibStrncasecmp,a,b,5);LEAF(sceClibStrnlen,a,3);
 LEAF(sceClibMemcmp,a,b,0);LEAF(sceClibStrncmp,a,b,0);LEAF(sceClibStrnlen,a,0);
 assert(e::stable_host(before,e::domain.stamp(ticket,0).ranges));
 assert(std::strcmp(a,"Alpha")==0&&std::strcmp(b,"alpha")==0);
 {e::HostLease outer;e::name_import("unreviewed_outer");
  observed::sceClibMemcmp(a,b,5);assert(e::domain.stamp(ticket,0).ranges.unknown_busy==1);}
 assert(!e::stable_host(before,e::domain.stamp(ticket,0).ranges));
}
void edges(const char *report) {
 prepare();e::enabled=true;checking=true;current_base=observed_bytes.data();
 std::array<float,32> src{};SceGxmProgramParameter p{SCE_GXM_PARAMETER_CATEGORY_UNIFORM,SCE_GXM_PARAMETER_TYPE_F32,8,3,4};
 // Nominal one component copies three in this CURRENT loop. Last actual byte
 // must be busy; next byte and vector-alignment holes must not be busy.
 late=true;{e::HostLease h;e::name_import("sceGxmSetUniformDataF");
  observed::sceGxmSetUniformDataF(current_base,&p,0,1,src.data());}
 assert(e::domain.output_stats().capacity_bytes==12);
 assert(!e::domain.stamp(ticket,512).ranges.busy);
 auto before=e::domain.stamp(ticket,16).ranges;
 {e::HostLease outer;e::name_import("unreviewed_outer");
  observed::sceGxmSetUniformDataF(current_base,&p,0,1,src.data());
  assert(e::domain.stamp(ticket,16).ranges.unknown_busy==1);}
 assert(!e::stable_host(before,e::domain.stamp(ticket,16).ranges));
 try {e::HostLease h;e::name_import("sceGxmSetUniformDataF");throw_copy=true;
  observed::sceGxmSetUniformDataF(current_base,&p,0,1,src.data());assert(false);
 }catch(const std::runtime_error&){}throw_copy=false;
 assert(!e::domain.output_stats().active&&!e::domain.stamp(ticket,16).ranges.busy);
 auto starts=e::domain.output_stats().begins;
 {e::HostLease h;e::name_import("sceGxmSetUniformDataF");
  assert(observed::sceGxmSetUniformDataF(nullptr,&p,0,1,src.data())==SCE_GXM_ERROR_INVALID_POINTER);
  p.category=other_category;
  assert(observed::sceGxmSetUniformDataF(current_base,&p,0,1,src.data())==SCE_GXM_ERROR_INVALID_VALUE);}
 assert(e::domain.output_stats().begins==starts&&!e::domain.snapshot().failed);
 FILE *f=std::fopen(report,"w");assert(f);e::domain.write_ranges(f);std::fclose(f);
 e::enabled=false;p.category=SCE_GXM_PARAMETER_CATEGORY_UNIFORM;
 observed::sceGxmSetUniformDataF(current_base,&p,0,1,src.data());
 assert(e::domain.output_stats().begins==starts);
}
int main(int argc,char **argv){assert(argc==2);e::enabled=true;
 for(unsigned arrays:{1u,8u})for(unsigned c=1;c<=4;++c)for(unsigned o=0;o<8;++o)for(unsigned n=0;n<18;++n)
  compare(SCE_GXM_PARAMETER_TYPE_F32,c,arrays,o,n);
 // Avoid claiming fixes for pre-existing narrow scalar expansion/partial-copy
 // source overreads: exercise bounded conversion cases with complete vectors.
 for(int t=0;t<7;++t)for(unsigned c=2;c<=4;++c)for(unsigned vectors:{1u,2u,4u})
  compare(static_cast<SceGxmParameterType>(t),c,8,0,c*vectors);
 leaves();edges(argv[1]);std::printf("Uniform old/new parity: %llu cases; all 512 byte interests checked at each copy.\n",(unsigned long long)cases);}
'''


def check_report(report):
    base = load(ROOT / 'scripts/tests/test_switch_host_outputs.py', 'uniform_output_test')
    analyzer = load(ROOT / 'scripts/analyze-switch-epoch.py', 'uniform_analyzer')
    # Real serialized registry paired with explicit synthetic phase counters.
    e = dict(active_host_at_export=0, range_contracts=report,
             candidate_work=dict(version=1, units='logical_input_bytes_including_palette', phases=[
                 dict(index=i, hash_bytes=100, cpu_bytes=0, hle_bytes=0, submission_bytes=0) for i in range(4)]),
             phases=[dict(cpu_candidates=0) for _ in range(4)],
             range_phases=[dict(hle_candidates=0, submission_candidates=0) for _ in range(4)])
    w = dict(phases=[dict(hash_bytes=100) for _ in range(4)])
    issues = []; result = analyzer.analyze_candidate_work(e, w, issues)
    assert not issues, issues
    assert result['output_leases']['sources'][4] == 3 and not result['safe_to_skip_hash']
    for key, value in [('version', True), ('version', 3), ('sources', [0,0,0,0]),
                       ('sources', [0,0,0,0,2]), ('active', 1), ('authority', True)]:
        bad = copy.deepcopy(e); bad['range_contracts']['output_leases'][key] = value
        issues = []; analyzer.analyze_candidate_work(bad, w, issues); assert issues, (key, value)
    # A genuine v1 shape still parses, but never silently drops a v2 source.
    old = copy.deepcopy(e); output = old['range_contracts']['output_leases']
    output['version'] = 1; output['sources'] = [3,0,0,0]
    issues = []; analyzer.analyze_candidate_work(old, w, issues); assert not issues, issues
    return base


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cxx', default='c++')
    parser.add_argument('--require-source', action='store_true')
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    convert, old, new, leaf_old, leaf_new, operations = sources(args.require_source)
    assert old.count('        memcpy(') == new.count('        memcpy(') == 2
    def program(changed):
        return (CPP.replace('__CONVERT__', convert)
                .replace('__ORIGINAL__', old.replace('        memcpy(', '        checked_copy('))
                .replace('__CHANGED__', changed.replace('        memcpy(', '        checked_copy('))
                .replace('__LEAF_BEFORE__', leaf_old).replace('__LEAF_AFTER__', leaf_new))
    with tempfile.TemporaryDirectory() as tmp:
        d = Path(tmp)
        flags = [args.cxx, '-std=c++20', '-O1' if args.sanitize else '-O2', '-Wall', '-Wextra', '-Werror',
                 '-pthread', '-D__SWITCH__', '-DVITA3K_SWITCH_TRACELAB=1', '-I', str(ROOT / 'vita3k/util/include')]
        if args.sanitize:
            flags += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
        (d / 'test.cpp').write_text(program(new))
        subprocess.run(flags + [str(d / 'test.cpp'), '-o', str(d / 'test')], check=True)
        subprocess.run([str(d / 'test'), str(d / 'report.json')], check=True, timeout=45)
        base = check_report(json.loads((d / 'report.json').read_text()))
        # Missing either actual output site MUST fail while executing its copy.
        for i, op in enumerate(operations[1:]):
            mutated = new.replace(op['new'], op['old'], 1)
            assert mutated != new
            (d / 'broken.cpp').write_text(program(mutated))
            subprocess.run(flags + [str(d / 'broken.cpp'), '-o', str(d / 'broken')], check=True)
            result = subprocess.run([str(d / 'broken'), str(d / 'bad.json')], capture_output=True, timeout=45)
            assert result.returncode != 0 and b'Assertion' in result.stderr, (i, result.stderr)
        base.target_sdk_gate(d)
    print('Uniform/leaf PASS: actual copy extents, unchanged algorithm, same-value, adjacency/gaps, late palette, '
          'unknown outer, unwind, disabled mode, real serializer v1/v2 and missing-site negatives. Host adapters only.')


if __name__ == '__main__':
    main()
