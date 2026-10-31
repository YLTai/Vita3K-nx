// Continuous bounded-profile reuse. Not a performance benchmark.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <util/resource_reuse_run.h>
namespace switch_validation::reuse {
class Play {
public:
    HashCache<> &cache;
    bool started=false, stopped=false, failed=false;
    uint32_t blockers=0;
    uint64_t frames=0, refreshes=0;
    bool tracking_retired=false;
    uint64_t stop_tick=0, stores_at_stop=0, page_only_at_stop=0;
    const char *stop_reason="none";
    explicit Play(HashCache<> &c):cache(c){}
    void reset(){started=stopped=failed=tracking_retired=false;blockers=0;frames=refreshes=0;
        stop_tick=stores_at_stop=page_only_at_stop=0;stop_reason="none";}
    void begin(){if(started||stopped)return;cache.begin();cache.phase(0,Mode::Online);started=true;}
    bool active()const{return started&&!stopped;}
    void stop(bool error=false,uint32_t mask=0,const char *reason=nullptr){
        failed|=error;blockers|=mask;stopped=true;cache.stop();
        stop_reason=error?"failure":reason?reason:"source_policy";
    }
    void view(StatusSnapshot &s)const{
        const auto r=cache.report();s.resource_reuse=s.resource_play=true;s.run_id=started?1:0;
        s.reuse_calls=r.total.reused_calls;s.reuse_bytes=r.total.reused_bytes;
        s.reuse_audits=r.total.audits;s.reuse_diffs=r.total.mismatches;s.reuse_blockers=blockers;
        s.watch_failed=failed||r.failed;s.phase_count=0;s.samples=frames;
        s.reuse_tracking_retired=tracking_retired;
        s.stage=stopped?Stage::Stopped:started?Stage::Measuring:Stage::Startup;
        s.outcome=failed||r.failed?CaptureOutcome::Invalid:CaptureOutcome::Pending;
    }
    void write(FILE*f)const{
        const auto r=cache.report();
        std::fprintf(f,"{\"version\":1,\"continuous\":true,\"started\":%s,\"stopped\":%s,\"failed\":%s,\"frames\":%llu,\"refresh_checks\":%llu,\"blockers\":%u,\"scene_memo\":\"original\",\"first_reuse_requires_full_audit\":true,\"GPU_write_sources_reused\":false,\"total\":",
            started?"true":"false",stopped?"true":"false",failed||r.failed?"true":"false",(unsigned long long)frames,(unsigned long long)refreshes,blockers);
        Run::counts(f,r.total);
        std::fputs(",\"counterexamples\":[",f);
        for(uint64_t i=0;i<r.examples;++i){const auto &x=r.first_examples[i];std::fprintf(f,"%s{\"pixels\":%llu,\"pixel_bytes\":%llu,\"palette\":%llu,\"palette_bytes\":%llu,\"cached\":%llu,\"reference\":%llu}",i?",":"",(unsigned long long)x.input.pixels,(unsigned long long)x.input.pixel_bytes,(unsigned long long)x.input.palette,(unsigned long long)x.input.palette_bytes,(unsigned long long)x.cached,(unsigned long long)x.reference);}
        std::fprintf(f,"],\"counterexamples_dropped\":%llu,\"source_policy\":",(unsigned long long)r.examples_dropped);
        source_policy.write(f);
        std::fprintf(f,",\"retirement\":{\"version\":1,\"tracking_retired\":%s,\"reason\":\"%s\",\"stop_tick\":%llu,\"watched_stores\":%llu,\"page_only_stores\":%llu}}",
            tracking_retired?"true":"false",stop_reason,(unsigned long long)stop_tick,
            (unsigned long long)stores_at_stop,(unsigned long long)page_only_at_stop);
    }
};
}
