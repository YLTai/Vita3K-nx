#!/usr/bin/env python3
"""Actual resource reuse: validate measured work and compare STOCK vs REUSE.
Audit stages are excluded from performance. No timings inferred from bytes.
"""
import math

ORDER=['STOCK','AUDIT','REUSE','REUSE','AUDIT','STOCK']
FIELDS=['calls','reference_calls','reference_bytes','reused_calls','reused_bytes','audits','audit_bytes','mismatches','raced_hashes','exceptions']
def analyze(data):
    errors=[];warnings=[];r=data.get('resource_reuse',{});rows=r.get('phases',[])
    def check(ok,msg):
        if not ok:errors.append(msg)
    def integer(x):return type(x) is int and 0<=x<2**64
    check(data.get('capture_plan')=='resource-reuse-1' and r.get('version')==1,'Wrong integration schema')
    check(data.get('controls_complete') is True and data.get('tests_dropped')==0,'Automatic controls missing/failed/dropped')
    tests=data.get('tests',[])
    check(all(t.get('status') in ('pass','skip') for t in tests),'Automatic control failure')
    check(sum(t.get('suite')=='resource_reuse' and t.get('name')=='dynamic_core' and t.get('status')=='pass' for t in tests)==1,'Missing/duplicate private core control')
    check(r.get('production_path') is True and r.get('hash_before_reuse_audit') is True and r.get('GPU_write_sources_reused') is False,'Invalid source authority')
    check(r.get('scene_memo')=='stock_and_performance_only','Unfair stock/reuse memo configuration')
    check(r.get('completed') is True and r.get('interrupted') is False and r.get('failed') is False,'Incomplete/failed/interrupted run')
    hz=r.get('timebase_hz',0);check(integer(hz) and hz>0 and hz==data.get('timebase_hz'),'Invalid timebase')
    check([p.get('mode') for p in rows]==ORDER and [p.get('index') for p in rows]==list(range(6)),'Wrong six-stage order')
    sums={k:0 for k in FIELDS};qualified=[]
    for i,p in enumerate(rows):
        c=p.get('work',{});valid=all(integer(c.get(k)) for k in FIELDS)
        check(valid,f'Phase {i}: invalid work counters')
        if not valid:continue
        for k in FIELDS:sums[k]+=c[k]
        check(c['calls']==c['reference_calls']+c['reused_calls'],f'Phase {i}: calls do not reconcile')
        check(c['audits']<=c['reference_calls'] and c['audit_bytes']<=c['reference_bytes'],f'Phase {i}: impossible audit counts')
        check(not c['mismatches'] and not c['exceptions'],f'Phase {i}: actual mismatch/exception')
        if i not in (2,3):check(c['reused_calls']==c['reused_bytes']==0,f'Phase {i}: skip outside performance')
        if i not in (1,4):check(c['audits']==0,f'Phase {i}: audit outside audit stage')
        n=p.get('samples',0);ticks=p.get('ticks',0)
        q=p.get('complete') is True and p.get('capped') is False and integer(n) and n>=32 and integer(ticks) and hz and ticks>=4*hz
        qualified.append(bool(q))
        if not q:warnings.append(f'Phase {i}: insufficient comparable samples')
    check(sums=={k:r.get('total',{}).get(k) for k in FIELDS},'Total/per-stage ledger mismatch')
    examples=r.get('counterexamples',[])
    check(isinstance(examples,list) and integer(r.get('counterexamples_dropped')) and len(examples)+r.get('counterexamples_dropped',0)==sums['mismatches'],'Counterexample ledger mismatch')
    check(r.get('minimum_samples')==32 and r.get('minimum_seconds')==4 and r.get('maximum_seconds')==8,'Wrong measurement schedule')
    policy=r.get('source_policy',{})
    check(policy.get('GPU_writers_reused') is False and policy.get('veto_release')=='session_end_only','Invalid persistent exclusions')
    treatment=len(rows)==6 and sums['reused_calls']>0 and rows[1].get('work',{}).get('audits',0)>0 and rows[4].get('work',{}).get('audits',0)>0
    if not treatment:warnings.append('No usable performance treatment or missing before/after audit; do not request a repeat of the same static scene')
    if policy.get('global_denials'):warnings.append(f"Unsupported source profile, fallback used: mask={policy['global_denials']}")
    comparison=None
    if not errors and len(qualified)==6 and all(qualified) and treatment:
        def pooled(indices):return sum(rows[i]['samples'] for i in indices)*hz/sum(rows[i]['ticks'] for i in indices)
        a,b=pooled((0,5)),pooled((2,3));comparison={'stock_loops_per_second':a,'reuse_loops_per_second':b,'throughput_change_percent':100*(b/a-1),'ms_per_loop_change':1000/b-1000/a,'stock_end_start_drift_percent':100*((rows[5]['samples']/rows[5]['ticks'])/(rows[0]['samples']/rows[0]['ticks'])-1)}
    return dict(capture_plan='resource-reuse-1',evidence_valid=not errors,comparison_eligible=comparison is not None,comparison=comparison,actual_work=sums,source_policy=policy,exclusions=errors,warnings=warnings+['Only the bounded source profile is enabled. No GPU-written inputs are reused.','STOCK and REUSE share baseline instrumentation. This is not a comparison to an uninstrumented build.','Counters include settling; timing excludes settling and audit phases. Logical bytes are not DRAM bandwidth.','Zero mismatch in this session is not a claim of all-game correctness.'])
