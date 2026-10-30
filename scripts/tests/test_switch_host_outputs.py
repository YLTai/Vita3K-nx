#!/usr/bin/env python3
"""Concrete write lease and exact backend-body tests. Codec/FFmpeg adapters are
explicit host models; target source/SDK gates do not claim device execution."""
import argparse, copy, importlib.util, json, os, shutil, subprocess, tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]

def load(path, name):
    spec=importlib.util.spec_from_file_location(name,path)
    mod=importlib.util.module_from_spec(spec);spec.loader.exec_module(mod);return mod

def function(text, signature):
    begin=text.index(signature); first=text.index('{',begin); depth=1; pos=first+1
    while depth:
        depth+=(text[pos]=='{')-(text[pos]=='}');pos+=1
    return text[begin:pos]

RECEIVERS = {
'atrac9': r'''bool Atrac9DecoderState::receive(uint8_t *data, DecoderSize *size) {
    Atrac9CodecInfo *info = static_cast<Atrac9CodecInfo *>(atrac9_info);
    if (data) {
        memcpy(data, result.data(), info->frameSamples * info->channels * sizeof(uint16_t));
    }
    if (size) { size->samples = static_cast<uint32_t>(info->frameSamples); }
    return true;
}''',
'aac': r'''bool AacDecoderState::receive(uint8_t *data, DecoderSize *size) {
    assert(frame->format == AV_SAMPLE_FMT_FLTP);
    if (data) {
        int ret = swr_convert(swr, &data, frame->nb_samples, const_cast<const uint8_t **>(frame->extended_data), frame->nb_samples);
        assert(ret >= 0);
    }
    if (size) { size->samples = frame->nb_samples; }
    return true;
}''',
'mp3': r'''bool Mp3DecoderState::receive(uint8_t *data, DecoderSize *size) {
    AVFrame *frame = av_frame_alloc();
    int err = avcodec_receive_frame(context, frame);
    if (err < 0) {
        LOG_WARN("Error receiving Mp3 frame: {}.", log_hex(static_cast<uint32_t>(err)));
        av_frame_free(&frame);
        return false;
    }
    if (data) {
        int data_size = av_get_bytes_per_sample(context->sample_fmt);
        for (int i = 0; i < frame->nb_samples; i++) {
            for (int ch = 0; ch < context->ch_layout.nb_channels; ch++) {
                for (int j = 0; j < data_size; j++) {
                    data[((data_size + context->ch_layout.nb_channels) * i) + (ch * context->ch_layout.nb_channels) + j] = frame->data[ch][data_size * i + j];
                }
            }
        }
    }
    if (size) { size->samples = frame->nb_samples; }
    av_frame_free(&frame);
    return true;
}'''
}

def bodies(required):
    recipe=json.loads((ROOT/'patches/experiments/switch-host-outputs.json').read_text())
    result=[]; declaration=''
    for f in recipe['files']:
        path=ROOT/f['path']
        if required and not path.exists(): raise RuntimeError('Missing production source: '+f['path'])
        text=path.read_text() if path.exists() else ''
        if required:
            for op in f['replacements']:
                assert text.count(op['new'])==1, (f['path'],op['new'][:80])
        stem=path.stem
        if stem in RECEIVERS:
            sig='bool '+{'atrac9':'Atrac9','aac':'Aac','mp3':'Mp3'}[stem]+'DecoderState::receive('
            if text and 'HostOutputLease output' in text: body=function(text,sig)
            else:
                body=RECEIVERS[stem]
                for op in f['replacements']:
                    if op['old'] in body: body=body.replace(op['old'],op['new'],1)
            assert 'HostOutputLease output' in body
            result.append(body)
        if f['path'].endswith('SceAudiodecUser.cpp'):
            # The production anchor now includes the unique helper prefix.
            # Extract only the inserted conditional, not a nested function body.
            replacement=f['replacements'][1]['new']
            begin=replacement.index('#if defined(__SWITCH__) && defined(VITA3K_SWITCH_TRACELAB)')
            end=replacement.index('    uint8_t *es_data',begin)
            declaration=replacement[begin:end]
            assert declaration.count('#if')==declaration.count('#endif')==1
            assert 'static int decode_audio_frames' not in declaration
            if required: assert text.count(declaration)==1
    assert len(result)==3 and 'typeid' in declaration
    return '\n'.join(result), declaration

CPP=r'''
#include <util/host_output_scope.h>
#include <cassert>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>
#include <stdexcept>
#include <typeinfo>
namespace e=switch_validation::epoch;
#define LOG_WARN(...) ((void)0)
struct DecoderSize{uint32_t samples=0;};
struct DecoderState{virtual ~DecoderState()=default;};
struct Atrac9CodecInfo{int frameSamples=0,channels=0;};
struct AVChannelLayout{int nb_channels=0;};
struct AVFrame{int format=1,nb_samples=0;uint8_t**extended_data=nullptr;uint8_t*data[8]{};};
struct AVContext{AVChannelLayout ch_layout;int sample_fmt=0;};
struct FakeSwr{int channels=0,query_result=0;};
constexpr int AV_SAMPLE_FMT_FLTP=1;
int av_opt_get_chlayout(void *s,const char*,int,AVChannelLayout*out){auto &a=*static_cast<FakeSwr*>(s);out->nb_channels=a.channels;return a.query_result;}
void av_channel_layout_uninit(AVChannelLayout*p){p->nb_channels=0;}
AVFrame shared_frame;int receive_error=0;uint64_t ticket=0;
AVFrame* av_frame_alloc(){return &shared_frame;}
void av_frame_free(AVFrame**p){*p=nullptr;}
int avcodec_receive_frame(AVContext*,AVFrame*){return receive_error;}
int av_get_bytes_per_sample(int f){return f;}
int swr_convert(void*s,uint8_t**out,int n,const uint8_t**,int){
 if(!e::domain.snapshot().failed)assert(e::domain.stamp(ticket,0).ranges.busy);
 std::memset(*out,0x66,size_t(n)*static_cast<FakeSwr*>(s)->channels*2);return n;}
struct Atrac9DecoderState:DecoderState{void*atrac9_info=nullptr;std::vector<uint8_t>result;bool receive(uint8_t*,DecoderSize*);};
struct AacDecoderState:DecoderState{AVFrame*frame=nullptr;void*swr=nullptr;bool receive(uint8_t*,DecoderSize*);};
struct Mp3DecoderState:DecoderState{AVContext*context=nullptr;bool receive(uint8_t*,DecoderSize*);};
struct Ctrl{uint32_t counts[10]{};};
__BODIES__
void actual_declaration(DecoderState*decoder,Ctrl*ctrl){
__DECLARATION__
}
void ranges(){
 e::Domain d;std::array<uint8_t,16384> b{};Ctrl ctrl;
 d.watch_range(0,b.data()+16,16,nullptr,0);auto t=d.publish();auto before=d.stamp(t,0).ranges;
 {e::HostLease root(&d,1);e::name_import("sceAudiodecDecode");assert(e::declare_outputs({"sceAudiodecDecode"},{{&ctrl,sizeof(ctrl)}}));
  {e::HostOutputLease w(b.data()+32,{8,true},e::OutputSource::Atrac9,&d);
   assert(e::stable_host(before,d.stamp(t,0).ranges));} // half-open adjacent bytes
  {e::HostOutputLease w(b.data()+15,{2,true},e::OutputSource::Atrac9,&d);
   {e::NativeWaitLease wait;assert(d.stamp(t,0).ranges.busy==1);}
   assert(d.stamp(t,0).ranges.busy==1);std::memset(b.data()+15,0,2); // same value still a write event
   {e::HostOutputLease another(b.data()+16,{8,true},e::OutputSource::Mp3,&d);assert(d.stamp(t,0).ranges.busy==2);}
   assert(d.stamp(t,0).ranges.busy==1);}
  assert(!e::stable_host(before,d.stamp(t,0).ranges));before=d.stamp(t,0).ranges;
  assert(e::stable_host(before,before));}
 // A new or remapped palette is immediately busy under a live independent output.
 {e::HostOutputLease out(b.data()+4090,{24,true},e::OutputSource::Generic,&d);
  d.watch_range(1,b.data()+100,4,b.data()+4096,4);assert(d.stamp(t,1).ranges.busy==1);
  d.clear_interests();d.watch_range(0,b.data()+4096,4,nullptr,0);assert(d.stamp(t,0).ranges.busy==1);
  std::thread worker([lease=std::move(out)]()mutable{});worker.join();assert(!d.stamp(t,0).ranges.busy);}
 // Stack unwinding ends a concrete write even without any HLE import owner.
 try{e::HostOutputLease out(b.data()+4096,{4,true},e::OutputSource::Generic,&d);throw std::runtime_error("test");}catch(const std::runtime_error&){}
 auto st=d.output_stats();assert(st.begins==st.ends&&!st.active&&!d.snapshot().failed);
 assert(d.reset());
 // Independent operations are bounded and fail closed; no reused stale slot.
 std::vector<std::unique_ptr<e::HostOutputLease>> leases;
 for(unsigned i=0;i<129;++i)leases.push_back(std::make_unique<e::HostOutputLease>(b.data(),e::WriteExtent{4,true},e::OutputSource::Generic,&d));
 assert(d.snapshot().failed);leases.clear();assert(d.output_stats().begins==d.output_stats().ends);assert(d.reset());
 {e::HostOutputLease bad(nullptr,{4,true},e::OutputSource::Aac,&d);assert(d.snapshot().failed);}assert(d.reset());
 {e::HostOutputLease bad(reinterpret_cast<void*>(UINTPTR_MAX-2),{4,true},e::OutputSource::Aac,&d);assert(d.snapshot().failed);}assert(d.reset());
 {e::HostOutputLease empty(nullptr,{0,true},e::OutputSource::Generic,&d);assert(!d.snapshot().failed);}
 d.watch_range(0,b.data(),4096,nullptr,0);t=d.publish();std::vector<std::thread>workers;
 for(unsigned k=0;k<4;++k)workers.emplace_back([&,k]{for(unsigned i=0;i<2048;++i){e::HostOutputLease out(b.data()+k*32,{16,true},e::OutputSource::Generic,&d);}});
 for(auto &w:workers){w.join();}
 st=d.output_stats();assert(st.begins==8192&&st.ends==8192&&!st.active&&!d.stamp(t,0).ranges.busy&&!d.snapshot().failed);
}
void extents(){
 assert(!e::packed_pcm_extent(-1,2,2).valid);assert(!e::packed_pcm_extent(INT64_MAX,INT64_MAX,8).valid);
 assert(e::packed_pcm_extent(0,2,2).valid&&e::packed_pcm_extent(0,2,2).bytes==0);
 for(int s=0;s<65;++s)for(int c=1;c<9;++c)for(int w:{1,2,4,8}){
  uint64_t last=0;for(int i=0;i<s;++i)for(int ch=0;ch<c;++ch)for(int j=0;j<w;++j)last=std::max(last,uint64_t((w+c)*i+ch*c+j+1));
  auto b=e::mp3_loop_extent(s,c,w);assert(b.valid&&b.bytes==last);
 }
 assert(!e::mp3_loop_extent(INT_MAX,8,8).valid);
 assert(e::mp3_loop_extent(3,1,2).bytes==8); // packed samples*channels*width=6 would miss two bytes
}
void backend_bodies(const char*file){
 auto &d=e::domain;e::enabled=true;std::array<uint8_t,256>b{};DecoderSize size;Ctrl ctrl;
 assert(d.reset());d.watch_range(0,b.data(),b.size(),nullptr,0);ticket=d.publish();
 Atrac9CodecInfo info{16,2};Atrac9DecoderState at;at.atrac9_info=&info;at.result.assign(64,0x55);
 {e::HostLease root(&d,101);e::name_import("sceAudiodecDecode");actual_declaration(&at,&ctrl);
  assert(!d.stamp(ticket,0).ranges.unknown_busy);auto a=d.stamp(ticket,0).ranges;
  assert(at.receive(b.data(),&size));assert(size.samples==16&&!std::memcmp(b.data(),at.result.data(),64));
  assert(!e::stable_host(a,d.stamp(ticket,0).ranges));assert(!d.stamp(ticket,0).ranges.busy);}
 struct Derived:Atrac9DecoderState{}derived;
 {e::HostLease root(&d,102);e::name_import("sceAudiodecDecode");actual_declaration(&derived,&ctrl);assert(d.stamp(ticket,0).ranges.unknown_busy);}
 {e::HostLease root(&d,103);e::name_import("unreviewed_outer");actual_declaration(&at,&ctrl);assert(d.stamp(ticket,0).ranges.unknown_busy);}
 FakeSwr swr{2,0};AVFrame frame;frame.nb_samples=16;AacDecoderState aa;aa.frame=&frame;aa.swr=&swr;
 {e::HostLease root(&d,104);e::name_import("sceAudiodecDecodeNFrames");actual_declaration(&aa,&ctrl);assert(aa.receive(b.data(),&size));}
 std::array<uint8_t,64>channels[2];channels[0].fill(0x12);channels[1].fill(0x34);
 shared_frame.nb_samples=3;shared_frame.data[0]=channels[0].data();shared_frame.data[1]=channels[1].data();
 AVContext context{{1},2};Mp3DecoderState mp;mp.context=&context;
 {e::HostLease root(&d,105);e::name_import("sceAudiodecDecode");actual_declaration(&mp,&ctrl);assert(mp.receive(b.data(),&size));}
 assert(d.output_stats().sources[1]==1&&d.output_stats().sources[2]==1&&d.output_stats().sources[3]==1);
 assert(d.output_stats().capacity_bytes==64+64+8);assert(!d.snapshot().failed);
 auto before=d.output_stats().begins;assert(at.receive(nullptr,&size));receive_error=-1;assert(!mp.receive(b.data(),&size));receive_error=0;assert(d.output_stats().begins==before);
 FILE*f=std::fopen(file,"w");assert(f);d.write_ranges(f);std::fclose(f);
 // Query failure invalidates the audit, never certifies an unbounded write.
 swr.query_result=-1;assert(aa.receive(b.data(),&size));assert(d.snapshot().failed);assert(d.reset());e::enabled=false;
}
int main(int argc,char**argv){assert(argc==2);ranges();extents();backend_bodies(argv[1]);}
'''

def check_report(report):
    a=load(ROOT/'scripts/analyze-switch-epoch.py','output_analyzer')
    # Use actual serialized metadata with synthetic per-phase workload, not
    # claimed GPU/Horizon execution. Both cache touch ledgers must reconcile.
    e=dict(active_host_at_export=0,range_contracts=report,
      candidate_work=dict(version=1,units='logical_input_bytes_including_palette',phases=[dict(index=i,hash_bytes=100,cpu_bytes=0,hle_bytes=0,submission_bytes=0)for i in range(4)]),
      phases=[dict(cpu_candidates=0)for _ in range(4)],range_phases=[dict(hle_candidates=0,submission_candidates=0)for _ in range(4)])
    w=dict(phases=[dict(hash_bytes=100)for _ in range(4)]);issues=[]
    result=a.analyze_candidate_work(e,w,issues);assert not issues,issues
    assert result['output_leases']['begins']==3 and not result['safe_to_skip_hash']
    for key,value in [('version',True),('authority',True),('ends',2),('active',1),('capacity_bytes',0),('sources',[0,1,1,0])]:
        bad=copy.deepcopy(e);bad['range_contracts']['output_leases'][key]=value;issues=[];a.analyze_candidate_work(bad,w,issues);assert issues,key
    bad=copy.deepcopy(e);bad['range_contracts']['imports'][0]['unknown_finishes']+=1
    issues=[];a.analyze_candidate_work(bad,w,issues);assert issues
    bad=copy.deepcopy(e);del bad['range_contracts']['output_leases'];issues=[];a.analyze_candidate_work(bad,w,issues);assert issues


def target_sdk_gate(tmp):
    """Compile against the installed target FFmpeg API, never a guessed ABI."""
    cross=shutil.which('aarch64-none-elf-g++')
    sdk=Path(os.environ.get('DEVKITPRO','/opt/devkitpro'))
    ff=sdk/'portlibs/switch/include'
    if not cross or not (ff/'libavutil/opt.h').exists():
        print('Target FFmpeg API gate SKIP: devkitA64/target headers unavailable; full NRO CI remains required.')
        return
    source=tmp/'target-output-api.cpp'
    source.write_text(r"""
#include <util/host_output_scope.h>
extern "C" {
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
}
void audit_output(void *swr, void *data, int samples) {
    AVChannelLayout layout{};
    const int rc = av_opt_get_chlayout(swr, "out_chlayout", 0, &layout);
    const auto extent = rc >= 0
        ? switch_validation::epoch::packed_pcm_extent(samples, layout.nb_channels, sizeof(int16_t))
        : switch_validation::epoch::WriteExtent{0, false};
    av_channel_layout_uninit(&layout);
    switch_validation::epoch::HostOutputLease output(data, extent,
        switch_validation::epoch::OutputSource::Aac);
}
""")
    flags=[cross,'-std=c++20','-O2','-fno-lto','-D_GNU_SOURCE','-D__SWITCH__',
        '-DVITA3K_SWITCH_TRACELAB=1','-march=armv8-a+crc+crypto','-mtune=cortex-a57',
        '-mtp=soft','-ftls-model=local-exec','-I',str(sdk/'libnx/include'),
        '-I',str(ff),'-I',str(ROOT/'vita3k/util/include')]
    subprocess.run(flags+['-c',str(source),'-o',str(tmp/'target-output-api.o')],check=True,timeout=45)
    print('Target FFmpeg output API and real lease header compiled; not final link or decoder execution.')

def main():
    p=argparse.ArgumentParser();p.add_argument('--cxx',default='c++');p.add_argument('--sanitize',action='store_true');p.add_argument('--require-source',action='store_true');a=p.parse_args()
    backend,dec=bodies(a.require_source)
    with tempfile.TemporaryDirectory()as tmp:
        d=Path(tmp);(d/'test.cpp').write_text(CPP.replace('__BODIES__',backend).replace('__DECLARATION__',dec))
        cmd=[a.cxx,'-std=c++20','-O1'if a.sanitize else '-O2','-Wall','-Wextra','-Werror','-pthread','-D__SWITCH__','-DVITA3K_SWITCH_TRACELAB=1','-I',str(ROOT/'vita3k/util/include')]
        if a.sanitize:cmd+=['-fsanitize=address,undefined','-fno-omit-frame-pointer']
        subprocess.run(cmd+[str(d/'test.cpp'),'-o',str(d/'test')],check=True)
        subprocess.run([str(d/'test'),str(d/'report.json')],check=True,timeout=45)
        check_report(json.loads((d/'report.json').read_text()))
        target_sdk_gate(d)
    print('Concrete host outputs PASS: actual ATRAC9/AAC/MP3 receive bodies with explicit codec adapters; dynamic busy/version, same-value, exact adjacent ranges, late palette/reset, moved worker lease, unwind, capacity/overflow/query failure, 8192 concurrent scopes and actual serializer/analyzer negatives. Not native decoder/GPU execution.')
if __name__=='__main__':main()
