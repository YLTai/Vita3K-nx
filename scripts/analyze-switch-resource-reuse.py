#!/usr/bin/env python3
"""Actual resource reuse: validate measured work and compare STOCK vs REUSE.
Audit stages are excluded from performance. No timings inferred from bytes.
"""
import math

ORDER=['STOCK','AUDIT','REUSE','REUSE','AUDIT','STOCK']
FALLBACK=['none','invalid_input','unsupported','busy','stale_lifetime','cold','version_changed','not_audited','concurrent_change','capacity','disabled']
DENIALS={1:'shader_buffer_store',2:'debugger',4:'capacity',8:'invalid_range',16:'backend',32:'native_network_work'}
FIELDS=['calls','reference_calls','reference_bytes','reused_calls','reused_bytes','audits','audit_bytes','mismatches','raced_hashes','exceptions']
def analyze(data):
    errors=[];warnings=[];r=data.get('resource_reuse',{});rows=r.get('phases',[])
    def check(ok,msg):
        if not ok:errors.append(msg)
    def integer(x):return type(x) is int and 0<=x<2**64
    check(data.get('capture_plan')=='resource-reuse-1' and type(r.get('version')) is int and r.get('version') in (1,2,3),'Wrong integration schema')
    check(data.get('controls_complete') is True and data.get('tests_dropped')==0,'Automatic controls missing/failed/dropped')
    tests=data.get('tests',[])
    check(all(t.get('status') in ('pass','skip') for t in tests),'Automatic control failure')
    check(sum(t.get('suite')=='resource_reuse' and t.get('name')=='dynamic_core' and t.get('status')=='pass' for t in tests)==1,'Missing/duplicate private core control')
    check(r.get('production_path') is True and r.get('hash_before_reuse_audit') is True and r.get('GPU_write_sources_reused') is False,'Invalid source authority')
    check(r.get('scene_memo')==('all_stages' if r.get('version')==3 else 'stock_and_performance_only'),'Unfair stock/reuse memo configuration')
    check(r.get('completed') is True and r.get('interrupted') is False and r.get('failed') is False,'Incomplete/failed/interrupted run')
    hz=r.get('timebase_hz',0);check(integer(hz) and hz>0 and hz==data.get('timebase_hz'),'Invalid timebase')
    check([p.get('mode') for p in rows]==ORDER and [p.get('index') for p in rows]==list(range(6)),'Wrong six-stage order')
    sums={k:0 for k in FIELDS};qualified=[];fallback=[0]*len(FALLBACK)
    for i,p in enumerate(rows):
        c=p.get('work',{});valid=all(integer(c.get(k)) for k in FIELDS)
        check(valid,f'Phase {i}: invalid work counters')
        if not valid:continue
        for k in FIELDS:sums[k]+=c[k]
        fb=c.get('fallback',[])
        check(isinstance(fb,list) and len(fb)==len(FALLBACK) and all(integer(x) for x in fb),f'Phase {i}: invalid fallback counts')
        if isinstance(fb,list) and len(fb)==len(FALLBACK) and all(integer(x) for x in fb):
            # A pre-scan miss and a detected in-scan race describe the SAME call.
            check(fb[0]==0 and fb[8]==c['raced_hashes'] and c['raced_hashes']<=c['reference_calls']
                  and sum(fb)-fb[8]<=c['reference_calls'],f'Phase {i}: impossible fallback count')
            fallback=[a+b for a,b in zip(fallback,fb)]
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
    check(fallback==r.get('total',{}).get('fallback'),'Fallback total/per-stage mismatch')
    if r.get('version')==3:
        fields=[0]*9
        for i,p in enumerate(rows):
            v=p.get('work',{}).get('version_fields')
            valid=isinstance(v,list) and len(v)==9 and all(integer(x) for x in v)
            check(valid,f'Phase {i}: missing version-change evidence')
            if valid:
                n=p['work']['fallback'][6]
                check(all(x<=n for x in v) and sum(v)>=n,f'Phase {i}: impossible version-change evidence')
                fields=[a+b for a,b in zip(fields,v)]
        check(fields==r.get('total',{}).get('version_fields'),'Version-field total/per-stage mismatch')

    examples=r.get('counterexamples',[])
    check(isinstance(examples,list) and integer(r.get('counterexamples_dropped')) and len(examples)+r.get('counterexamples_dropped',0)==sums['mismatches'],'Counterexample ledger mismatch')
    check(r.get('minimum_samples')==32 and r.get('minimum_seconds')==4 and r.get('maximum_seconds')==8,'Wrong measurement schedule')
    policy=r.get('source_policy',{})
    check(policy.get('GPU_writers_reused') is False and policy.get('veto_release')=='session_end_only','Invalid persistent exclusions')
    policy_mask=policy.get('global_denials',0)
    policy_mask_valid=type(policy_mask) is int and 0<=policy_mask<2**32
    check(policy_mask_valid,'Invalid global policy mask')
    termination=r.get('termination',{})
    early=False
    if r.get('version') in (2,3):
        check(isinstance(termination,dict),'Missing v2 termination record')
        if not isinstance(termination,dict):termination={}
        why,phase,mask=termination.get('reason'),termination.get('phase'),termination.get('denials')
        valid=why in ('none','global_policy','no_auditable_inputs','no_reused_inputs') and type(phase) is int and 0<=phase<6 and type(mask) is int and 0<=mask<2**32
        check(valid,'Invalid termination record')
        if valid:
            early=why!='none'
            check(mask==0 if why!='global_policy' else mask!=0,'Termination denial/reason mismatch')
            if not early:check(phase==0,'Unused stop phase must be zero')
            if early:
                check(data.get('capture_outcome')=='inconclusive','Early no-treatment is not a correctness pass')
                check(len(rows)==6,'Missing stopped phases')
                if len(rows)==6:
                    for j in range(phase):check(rows[j].get('complete') is True,'Earlier phase not complete')
                    for row in rows[phase+1:]:
                        check(row.get('complete') is False and all(row.get(k)==0 for k in ('samples','ticks','begin_tick','end_tick'))
                              and all(row.get('work',{}).get(k)==0 for k in FIELDS),'Work after terminal no-treatment')
                    if why=='no_auditable_inputs':check(phase==1 and rows[1].get('complete') is True and rows[1]['work']['audits']==0,'Invalid no-audit stop')
                    if why=='no_reused_inputs':check(phase==3 and rows[3].get('complete') is True and not sums['reused_calls'],'Invalid no-reuse stop')
                if why=='global_policy':
                    effective=(policy_mask if policy_mask_valid else 0) | (0 if policy.get('backend_supported') else 16)
                    check((mask & effective)==mask,'Stop reason absent from source policy')
                warnings.append('Stopped automatically: '+why+'; partial evidence retained, no performance conclusion')
    treatment=not early and len(rows)==6 and sums['reused_calls']>0 and rows[1].get('work',{}).get('audits',0)>0 and rows[4].get('work',{}).get('audits',0)>0
    if not treatment:warnings.append('No usable performance treatment or missing before/after audit; do not request a repeat of the same static scene')
    labels=[label for bit,label in DENIALS.items() if policy_mask_valid and policy_mask & bit]
    if policy_mask:warnings.append(f"Unsupported source profile, fallback used: mask={policy_mask}, reasons={','.join(labels)}")
    comparison=None
    if not errors and len(qualified)==6 and all(qualified) and treatment:
        def pooled(indices):return sum(rows[i]['samples'] for i in indices)*hz/sum(rows[i]['ticks'] for i in indices)
        a,b=pooled((0,5)),pooled((2,3));comparison={'stock_loops_per_second':a,'reuse_loops_per_second':b,'throughput_change_percent':100*(b/a-1),'ms_per_loop_change':1000/b-1000/a,'stock_end_start_drift_percent':100*((rows[5]['samples']/rows[5]['ticks'])/(rows[0]['samples']/rows[0]['ticks'])-1)}
    return dict(capture_plan='resource-reuse-1',evidence_valid=not errors,comparison_eligible=comparison is not None,comparison=comparison,actual_work=sums,source_policy=policy,denial_reasons=labels,termination=termination if r.get('version') in (2,3) else None,fallback_counts=dict(zip(FALLBACK,fallback)),exclusions=errors,warnings=warnings+['Only the bounded source profile is enabled. No GPU-written inputs are reused.','STOCK and REUSE share baseline instrumentation. This is not a comparison to an uninstrumented build.','Counters include settling; timing excludes settling and audit phases. Logical bytes are not DRAM bandwidth.','Zero mismatch in this session is not a claim of all-game correctness.'])
