#!/usr/bin/env python3
"""Fail-closed analysis of actual-game publication-grace audit, never reuse authority."""
import math

CONTROLS = {
    'first_write_reopens_aliases', 'open_page_repeat_negative_control',
    'native_exact_store', 'native_repeat_alias_store', 'native_same_value_store',
    'raw_after_cpu_negative_control', 'page_neighbor_negative_control',
    'host_notice_positive_control', 'observer_CAS_contract', 'cache_first_baseline_ledger',
    'cache_shared_history_ledger', 'remap_discards_inflight_hash',
    'native_palette_only_store', 'native_crosspage_exact_ranges', 'canary_cleanup',
}

def integer(o, k):
    n = o[k]
    if type(n) is not int or n < 0:
        raise ValueError('invalid nonnegative integer: ' + k)
    return n

def analyze(data, changed_total=None):
    issues, warnings, rows = [], [], []
    b, w, e = (data.get(k, {}) for k in ('benchmark', 'write_shadow', 'epoch_audit'))
    tests = data.get('tests', [])
    controls = [t for t in tests if t.get('suite') == 'write_watch_canary']
    controls_ok = len(controls) == len(CONTROLS) and all(
        sum(t.get('name') == name and t.get('status') == 'pass' for t in controls) == 1 for name in CONTROLS)
    if not controls_ok or data.get('controls_complete') is not True:
        issues.append('Missing, duplicated, or failed mutation controls')
    if data.get('tests_dropped') != 0 or any(t.get('status') not in ('pass', 'skip') for t in tests):
        issues.append('Failed or lost startup results')
    if data.get('capture_plan') != 'epoch-write-1' or data.get('hash_skipping') is not False or data.get('strict_hash') is not True:
        issues.append('Wrong plan or full-Hash reference disabled')
    if any(e.get(k) is not False for k in ('absence_authorizes_hash_skip', 'GPU_completion_covered', 'async_HLE_covered')):
        issues.append('Unsupported authority/coverage claim')
    if b.get('order') != 'ABBA' or b.get('experiment') != 'epoch_write_audit':
        issues.append('Wrong treatment order')
    if b.get('completed') is not True or b.get('aborted') is not False or b.get('frames_overflow') != 0:
        issues.append('Capture incomplete, interrupted, or overflowed')
    if w.get('bound') is not True or w.get('failed') is not False or e.get('failed') is not False:
        issues.append('Observer or execution-domain unavailable/failed')
    topology = data.get('guest_backing', {})
    imports = topology.get('deferred_imports', {})
    if topology.get('active') is not True or not topology.get('pool_bytes') or topology.get('errors'):
        issues.append('Direct Heap disabled or backing error')
    if imports.get('enabled') is not True or imports.get('frozen_at_window_end') is not True or imports.get('errors'):
        issues.append('Deferred imports unavailable, failed, or no capture-end snapshot')
    comparison = None
    totals = {k: 0 for k in ('grace_ready', 'grace_pending', 'cpu_candidates', 'cpu_candidate_changes',
        'epoch_candidates', 'epoch_candidate_changes', 'external_intervals', 'host_busy_checks')}
    try:
        if integer(b, 'settle_seconds') != 1 or integer(b, 'measure_seconds') != 6:
            raise ValueError('Unexpected phase timing')
        for a, z, active in [('jit_entries','jit_exits','active_jits_at_export'), ('host_entries','host_exits','active_host_at_export')]:
            if integer(e,a) != integer(e,z) + integer(e,active):
                raise ValueError('Execution lease accounting mismatch')
            if integer(e,active): issues.append('Export taken before all tracked workers stopped')
        if not integer(e,'jit_entries') or integer(e,'publications') < 2:
            issues.append('No actual JIT/publication evidence')
        if not integer(e,'host_entries') or not integer(e,'gpu_boundaries'):
            issues.append('No HLE or GPU submission-boundary evidence')
        if len(b.get('phases',[])) != 4 or len(w.get('phases',[])) != 4 or len(e.get('phases',[])) != 4:
            raise ValueError('Exactly four phase records required')
        for i, (p,c,g) in enumerate(zip(b['phases'],w['phases'],e['phases'])):
            if [p.get('index'),c.get('index'),g.get('index')] != [i,i,i]:
                raise ValueError('Phase identity mismatch')
            mode = 'exact_range' if i in (1,2) else 'baseline'
            if p.get('variant') != mode: raise ValueError('Incorrect phase variant')
            count = integer(p,'measured_intervals'); duration = p['duration_ms']; rate = p['loops_per_second']
            if type(duration) not in (int,float) or not math.isfinite(duration) or duration <= 0:
                raise ValueError('Invalid duration')
            if type(rate) not in (int,float) or not math.isfinite(rate) or not math.isclose(rate, count*1000/duration,rel_tol=1e-5):
                raise ValueError('Rate inconsistent with samples')
            if p.get('complete') is not True or count < 50 or not 5400 <= duration <= 6000:
                issues.append(f'Phase {i}: insufficient samples/time')
            if integer(g,'end_tick') <= integer(g,'begin_tick') or (i and g['begin_tick'] != e['phases'][i-1]['end_tick']):
                raise ValueError('Invalid source-render boundaries')
            for k in totals: totals[k] += integer(g,k)
            if g['epoch_candidates'] + g['external_intervals'] != g['cpu_candidates']:
                raise ValueError('Candidate classification does not reconcile')
            if g['epoch_candidate_changes'] > g['cpu_candidate_changes'] or g['epoch_candidates'] > g['cpu_candidates'] or g['cpu_candidates'] > g['grace_ready']:
                raise ValueError('Impossible candidate counts')
            if g['epoch_candidate_changes'] > g['epoch_candidates'] or g['cpu_candidate_changes'] > g['cpu_candidates']:
                raise ValueError('More mismatches than checks')
            if integer(c,'guard_budget_exhausted') or integer(c,'invalid_accesses'):
                issues.append(f'Phase {i}: diagnostic budget/range failure')
            if i in (1,2) and (not integer(c,'armed_pages') or not g['grace_ready']):
                issues.append(f'Phase {i}: no grace-qualified monitored input')
            if i in (0,3) and any(g[k] for k in totals):
                raise ValueError('Baseline contains treatment observations')
            rows.append(dict(index=i, variant=mode, measured_intervals=count, duration_ms=duration,
                             loops_per_second=rate, observations_including_settle=g))
        mismatches = e.get('mismatches')
        if not isinstance(mismatches,list) or len(mismatches) > 32 or len(mismatches) + integer(e,'mismatches_dropped') != totals['epoch_candidate_changes']:
            raise ValueError('Mismatch detail ledger does not reconcile')
        for event in mismatches:
            i = integer(event,'phase')
            if i not in (1,2) or not integer(event,'publication') or integer(event,'resource') >= integer(w,'resources'):
                raise ValueError('Invalid mismatch identity')
            for key in ('previous_hash_hex','current_hash_hex'):
                if len(event[key]) != 16: raise ValueError('Invalid mismatch digest')
                int(event[key],16)
            if event['previous_hash_hex'] == event['current_hash_hex']:
                raise ValueError('Mismatch has equal digests')
        cache=w['cache_ledger']
        changes = integer(cache,'changes')
        if not isinstance(cache['reasons'],dict) or any(type(n) is not int or n < 0 for n in cache['reasons'].values()) or sum(cache['reasons'].values()) != changes:
            raise ValueError('Cache change reason ledger does not reconcile')
        if changed_total is not None and changes != changed_total:
            issues.append('Cache change ledger disagrees with full-session TraceLab')
    except (KeyError,TypeError,ValueError,ZeroDivisionError) as ex:
        issues.append(str(ex))
    if totals['epoch_candidate_changes']:
        warnings.append('Full Hash disproved stable-epoch candidates; inspect GPU completion, async HLE and missing write sources')
    if not totals['epoch_candidates']:
        warnings.append('No coarse-epoch candidates: broad HLE/GPU exclusions may dominate; do not repeat the same scene for green')
    if not issues and len(rows)==4:
        def group(indices):
            n=sum(rows[i]['measured_intervals'] for i in indices); ms=sum(rows[i]['duration_ms'] for i in indices)
            return dict(loops_per_second=1000*n/ms,mean_loop_ms=ms/n)
        a,bb=group((0,3)),group((1,2))
        comparison=dict(baseline=a,treatment=bb,throughput_delta_pct=(bb['loops_per_second']/a['loops_per_second']-1)*100,
            extra_ms_per_loop=bb['mean_loop_ms']-a['mean_loop_ms'],
            baseline_drift_pct=(rows[3]['loops_per_second']/rows[0]['loops_per_second']-1)*100, descriptive_only=True)
    return dict(plan='epoch-write-1',controls_ok=controls_ok,comparison_eligible=not issues,
        exclusions=issues,warnings=warnings,phases=rows,totals=totals,comparison=comparison,
        safe_to_skip_hash=False,next_action='Exit normally and share one complete capture; do not repeat for green',
        limits=['This is a real-game software-write-table audit, not the private CPU/GPU mirror suite.',
                'A and B share read/write tables, cross-page detection and execution/HLE boundary costs; B adds persistent selected-page interception.',
                'Only pre-publication JIT runs are drained; no renderer pause or global stop is performed.',
                'Generic HLE/GPU boundaries are conservative exclusions, not exact writer attribution or GPU completion proof.',
                'Async HLE, IPC and GPU visibility remain unproven. Every actual texture Hash still uses the reference.',
                'Counters include settling; timing excludes it. Zero mismatches is not complete writer coverage.'])
