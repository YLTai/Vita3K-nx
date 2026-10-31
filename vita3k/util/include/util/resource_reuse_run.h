// One integrated run: stock / audit / reuse / reuse / audit / stock.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <util/resource_hash_reuse.h>
#include <util/resource_source_policy.h>
#include <util/switch_validation_status.h>
#include <algorithm>
#include <cstdio>
namespace switch_validation::reuse {
class Run {
    uint64_t hz_ = 0, start_ = 0, previous_ = 0;
public:
    static constexpr unsigned count = 6, min_samples = 32, min_seconds = 4, max_seconds = 8;
    struct Timing { uint64_t samples=0, ticks=0, begin=0, end=0; bool complete=false, capped=false; };
    std::array<Timing,count> timing{};
    HashCache<> cache;
    uint32_t phase = 0, run_id = 0;
    bool active=false, done=false, interrupted=false, failure=false;
    enum StopReason : uint32_t { None, GlobalPolicy, NoAudit, NoReuse };
    StopReason stop_reason = None;
    uint32_t stop_phase = 0, stop_denials = 0;
    static constexpr const char *stop_name(StopReason reason) {
        return reason == GlobalPolicy ? "global_policy" : reason == NoAudit ? "no_auditable_inputs"
            : reason == NoReuse ? "no_reused_inputs" : "none";
    }
    void stop_without_treatment(StopReason reason, uint32_t denials = 0) {
        stop_reason = reason; stop_phase = phase; stop_denials = denials; finish();
    }
    static constexpr Mode mode(unsigned i) { return i==1 || i==4 ? Mode::Audit : i==2 || i==3 ? Mode::Performance : Mode::Reference; }
    static constexpr const char *name(unsigned i) { return i==1 || i==4 ? "AUDIT" : i==2 || i==3 ? "REUSE" : "STOCK"; }
    uint64_t last_tick() const { return previous_; }
    void reset() { active=done=interrupted=failure=false; phase=run_id=0; timing={}; stop_reason=None; stop_phase=stop_denials=0; cache.stop(); }
    void begin(uint64_t tick, uint64_t hz) {
        timing={}; stop_reason=None; stop_phase=stop_denials=0; phase=0; ++run_id; active=true; done=interrupted=failure=false;
        hz_=hz; start_=previous_=tick; timing[0].begin=tick;
        cache.begin(); cache.phase(0,mode(0));
        if (!hz || hz>UINT64_MAX/100000) finish(true);
        else if (const auto mask = source_policy.blockers()) stop_without_treatment(GlobalPolicy, mask);
    }
    void finish(bool failed=false, bool interrupt=false) {
        failure|=failed; interrupted|=interrupt; active=false; done=true; cache.stop();
    }
    // A phase ends only on an actual renderer callback, never from the HUD timer.
    bool frame(uint64_t tick) {
        if (!active) return false;
        if (tick<previous_ || cache.failed()) { finish(true); return true; }
        if (const auto mask = source_policy.blockers()) {
            stop_without_treatment(GlobalPolicy, mask); return true;
        }
        auto &t=timing[phase]; const auto elapsed=tick-start_;
        if (tick>previous_ && previous_>=start_+hz_ && elapsed<=9*hz_) { ++t.samples; t.ticks+=tick-previous_; }
        previous_=tick;
        const bool enough=t.samples>=min_samples && t.ticks>=min_seconds*hz_;
        if (!enough && elapsed<9*hz_) return false;
        t.complete=true; t.capped=!enough; t.end=tick;
        // Do not spend the rest of a hardware run on an optimization which
        // cannot execute. Preserve all partial evidence; this is inconclusive,
        // not a clean correctness or performance pass.
        const auto work = cache.report();
        if (phase == 1 && !work.phases[1].audits) {
            stop_without_treatment(NoAudit); return true;
        }
        if (phase == 3 && !work.phases[2].reused_calls && !work.phases[3].reused_calls) {
            stop_without_treatment(NoReuse); return true;
        }
        if (++phase==count) { finish(); return true; }
        start_=previous_=tick; timing[phase].begin=tick; cache.phase(phase,mode(phase)); return true;
    }
    void defer(uint64_t tick) { if (active && tick>start_) {start_=previous_=tick;timing[phase].begin=tick;} }
    void view(StatusSnapshot &out, uint64_t tick) const {
        const auto r=cache.report(); out.resource_reuse=true; out.run_id=run_id;
        out.phase_count=count; out.phase=active?phase+1:done?(stop_reason!=None?stop_phase+1:count):0;
        out.reuse_stop_reason=stop_reason; out.reuse_blockers=stop_denials;
        out.reuse_calls=r.total.reused_calls; out.reuse_bytes=r.total.reused_bytes;
        out.reuse_audits=r.total.audits; out.reuse_diffs=r.total.mismatches;
        out.phase_samples=active?timing[phase].samples:0;
        out.reuse_mode=active?unsigned(mode(phase)):0;
        out.samples=0; bool caps=false; for(const auto &t:timing) {out.samples+=t.samples;caps|=t.capped;}
        out.stage=active ? tick-start_<hz_?Stage::Settling:Stage::Measuring : done?Stage::Complete:Stage::Waiting;
        out.sample_extending=active && tick-start_>=5*hz_;
        out.remaining_ms=active ? ((count-phase)*9*hz_-std::min(tick-start_,9*hz_))*1000/hz_ : 0;
        out.progress_per_mille=done?1000:active?uint32_t((phase*1000+std::min<uint64_t>(999,(tick-start_)*1000/(9*hz_)))/count):0;
        out.outcome=!done?CaptureOutcome::Pending:failure||r.failed||interrupted?CaptureOutcome::Invalid:
            stop_reason!=None||caps||!r.total.reused_calls||!r.phases[4].audits?CaptureOutcome::Inconclusive:CaptureOutcome::Ready;
        out.watch_failed|=failure||r.failed; out.insufficient_samples=caps;
    }
    static void counts(FILE *f,const Counters &c) {
        std::fprintf(f,"{\"calls\":%llu,\"reference_calls\":%llu,\"reference_bytes\":%llu,\"reused_calls\":%llu,\"reused_bytes\":%llu,\"audits\":%llu,\"audit_bytes\":%llu,\"mismatches\":%llu,\"raced_hashes\":%llu,\"exceptions\":%llu,\"fallback\":[",
            (unsigned long long)c.calls,(unsigned long long)c.reference_calls,(unsigned long long)c.reference_bytes,
            (unsigned long long)c.reused_calls,(unsigned long long)c.reused_bytes,(unsigned long long)c.audits,
            (unsigned long long)c.audit_bytes,(unsigned long long)c.mismatches,(unsigned long long)c.raced_hashes,(unsigned long long)c.exceptions);
        for(size_t i=0;i<c.fallback.size();++i) { std::fprintf(f,"%s%llu",i?",":"",(unsigned long long)c.fallback[i]); }
        std::fputs("]}",f);
    }
    void write(FILE *f) const {
        const auto r=cache.report();
        std::fprintf(f,"{\"version\":2,\"production_path\":true,\"completed\":%s,\"failed\":%s,\"interrupted\":%s,\"run_id\":%u,\"hash_before_reuse_audit\":true,\"GPU_write_sources_reused\":false,\"scene_memo\":\"stock_and_performance_only\",\"timebase_hz\":%llu,\"minimum_samples\":%u,\"minimum_seconds\":%u,\"maximum_seconds\":%u,\"total\":",
            done?"true":"false",failure||r.failed?"true":"false",interrupted?"true":"false",run_id,(unsigned long long)hz_,min_samples,min_seconds,max_seconds);
        counts(f,r.total);
        std::fprintf(f,",\"termination\":{\"reason\":\"%s\",\"phase\":%u,\"denials\":%u}", stop_name(stop_reason), stop_phase, stop_denials);
        std::fputs(",\"phases\":[",f);
        for(unsigned i=0;i<count;++i){const auto&t=timing[i];std::fprintf(f,"%s{\"index\":%u,\"mode\":\"%s\",\"samples\":%llu,\"ticks\":%llu,\"begin_tick\":%llu,\"end_tick\":%llu,\"complete\":%s,\"capped\":%s,\"work\":",i?",":"",i,name(i),(unsigned long long)t.samples,(unsigned long long)t.ticks,(unsigned long long)t.begin,(unsigned long long)t.end,t.complete?"true":"false",t.capped?"true":"false");counts(f,r.phases[i]);std::fputc('}',f);}
        std::fputs("],\"counterexamples\":[",f);
        for(uint64_t i=0;i<r.examples;++i){const auto&e=r.first_examples[i];std::fprintf(f,"%s{\"phase\":%llu,\"pixels\":%llu,\"pixel_bytes\":%llu,\"palette\":%llu,\"palette_bytes\":%llu,\"cached\":%llu,\"reference\":%llu}",i?",":"",(unsigned long long)e.phase,(unsigned long long)e.input.pixels,(unsigned long long)e.input.pixel_bytes,(unsigned long long)e.input.palette,(unsigned long long)e.input.palette_bytes,(unsigned long long)e.cached,(unsigned long long)e.reference);}
        std::fprintf(f,"],\"counterexamples_dropped\":%llu,\"source_policy\":",(unsigned long long)r.examples_dropped);source_policy.write(f);std::fputc('}',f);
    }
};
} // namespace switch_validation::reuse
