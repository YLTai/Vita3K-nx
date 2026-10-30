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
    issues, warnings, rows, performance_issues = [], [], [], []
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
    scoped = None
    if "range_contracts" in e:
        scoped = analyze_ranges(e, w, issues, warnings)
    analyze_callback_polls(e, issues)
    comparison = None
    policy = data.get('sample_policy')
    adaptive = policy is not None
    capped = [False] * 4
    if adaptive:
        if (not isinstance(policy, dict) or type(policy.get('version')) is not int
                or policy.get('version') != 1 or policy.get('minimum_samples') != 50
                or policy.get('minimum_seconds') != 6 or policy.get('maximum_seconds') != 12
                or policy.get('settle_seconds') != 1):
            issues.append('Unsupported adaptive sampling contract')
        else:
            capped = policy.get('capped_phases')
            if not isinstance(capped, list) or len(capped) != 4 or any(type(x) is not bool for x in capped):
                issues.append('Invalid sample-cap ledger')
                capped = [True] * 4
        if 'native_waits' not in e.get('range_contracts', {}):
            issues.append('Adaptive wait-audit build lacks native-wait accounting')
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
            if p.get('complete') is not True:
                issues.append(f'Phase {i}: incomplete phase')
            valid_duration = 6000 - 1e-5 <= duration <= 12000 + 1e-5 if adaptive else 5400 <= duration <= 6000
            if count < 50 or not valid_duration or capped[i]:
                performance_issues.append(f'Phase {i}: insufficient samples/time' + (' (bounded extension exhausted)' if capped[i] else ''))
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
    if not totals['epoch_candidates'] and not (scoped and scoped['totals']['hle_candidates']):
        warnings.append('No coarse or scoped candidates: inspect blockers; do not repeat unchanged code for green')
    if not issues and not performance_issues and len(rows)==4:
        def group(indices):
            n=sum(rows[i]['measured_intervals'] for i in indices); ms=sum(rows[i]['duration_ms'] for i in indices)
            return dict(loops_per_second=1000*n/ms,mean_loop_ms=ms/n)
        a,bb=group((0,3)),group((1,2))
        comparison=dict(baseline=a,treatment=bb,throughput_delta_pct=(bb['loops_per_second']/a['loops_per_second']-1)*100,
            extra_ms_per_loop=bb['mean_loop_ms']-a['mean_loop_ms'],
            baseline_drift_pct=(rows[3]['loops_per_second']/rows[0]['loops_per_second']-1)*100, descriptive_only=True)
    return dict(plan='epoch-write-1',controls_ok=controls_ok,comparison_eligible=not issues and not performance_issues,
        evidence_eligible=not issues,performance_exclusions=performance_issues,sample_policy=policy,
        exclusions=issues+performance_issues,warnings=warnings,phases=rows,totals=totals,comparison=comparison,
        scoped_hle=scoped,safe_to_skip_hash=False,next_action='Exit normally and share one complete capture; do not repeat for green',
        limits=['This is a real-game software-write-table audit, not the private CPU/GPU mirror suite.',
                'A and B share read/write tables, cross-page detection and execution/HLE boundary costs; B adds persistent selected-page interception.',
                'Only pre-publication JIT runs are drained; no renderer pause or global stop is performed.',
                'Generic HLE/GPU boundaries are conservative exclusions, not exact writer attribution or GPU completion proof.',
                'Async HLE, IPC and GPU visibility remain unproven. Every actual texture Hash still uses the reference.',
                'Counters include settling; timing excludes it. Zero mismatches is not complete writer coverage.'])


def analyze_ranges(e, w, issues, warnings):
    """A supplement: evidence may improve without ever authorizing Hash reuse."""
    keys = ('hle_candidates','hle_candidate_changes','hle_unknown_blocks','hle_range_blocks',
            'hle_history_blocks','submission_candidates','submission_candidate_changes','gpu_epoch_blocks','unrelated_mapping_updates')
    total = {k:0 for k in keys}
    contracts = e.get('range_contracts', {})
    try:
        if contracts.get('version') != 1 or contracts.get('failed') is not False or contracts.get('authority') is not False:
            raise ValueError('Invalid scoped-HLE registry or unsupported authority')
        if contracts.get('attribution_period') != 64:
            raise ValueError('Unknown blocker-attribution sample period')
        if integer(contracts,'calls') != integer(e,'host_entries') or integer(contracts,'exits') != integer(e,'host_exits'):
            raise ValueError('Scoped/global HLE accounting mismatch')
        if integer(contracts,'scoped_calls') > contracts['calls']:
            raise ValueError('More declared contracts than imports')
        imports = contracts.get('imports')
        if not isinstance(imports,list) or len(imports)>256:
            raise ValueError('Invalid HLE caller table')
        native = contracts.get('native_waits')
        parks = resumes = 0
        if native is not None:
            if (native.get('version') != 1 or native.get('async_outputs_covered') is not False
                    or integer(native, 'parks') != integer(native, 'resumes') + integer(native, 'active')):
                raise ValueError('Invalid native-wait lease ledger/coverage')
            if native['active']:
                raise ValueError('Native wait still active at export; workers not stopped')
        seen=set();calls=scoped=0
        for row in imports:
            nid=integer(row,'nid')
            if nid in seen or nid > 0xffffffff: raise ValueError('Invalid/duplicated caller identity')
            seen.add(nid);n=integer(row,'calls');a=integer(row,'scoped')
            if not n or a>n or not isinstance(row.get('name'),str):raise ValueError('Invalid HLE caller counts')
            integer(row,'unknown_candidate_checks');integer(row,'overlap_candidate_checks')
            calls+=n;scoped+=a
            if native is not None:
                park = integer(row, 'parks'); resume = integer(row, 'resumes')
                integer(row, 'parked_candidate_checks')
                if resume > park: raise ValueError('More native-wait resumes than parks')
                parks += park; resumes += resume
        if calls+integer(contracts,'unattributed_calls') != contracts['calls'] or scoped>contracts['scoped_calls']:
            raise ValueError('Caller coverage ledger does not reconcile')
        if native is not None:
            if parks > native['parks'] or resumes > native['resumes']:
                raise ValueError('Caller/native-wait ledger disagrees')
            if not contracts['unattributed_calls'] and (parks != native['parks'] or resumes != native['resumes']):
                raise ValueError('Complete caller/native-wait ledger disagrees')
        phases=e.get('range_phases')
        if not isinstance(phases,list) or len(phases)!=4:raise ValueError('Missing four scoped-HLE phases')
        for i,(p,g) in enumerate(zip(phases,e['phases'])):
            if p.get('index')!=i:raise ValueError('Scoped-HLE phase identity mismatch')
            for k in keys:total[k]+=integer(p,k)
            if p['hle_candidates']+p['hle_unknown_blocks']+p['hle_range_blocks']+p['hle_history_blocks']!=integer(g,'cpu_candidates'):
                raise ValueError('Scoped-HLE candidate reasons do not reconcile')
            if p['submission_candidates']+p['gpu_epoch_blocks']!=p['hle_candidates']:
                raise ValueError('GPU submission-history classification does not reconcile')
            if p['hle_candidate_changes']>p['hle_candidates'] or p['submission_candidate_changes']>p['submission_candidates'] or p['submission_candidate_changes']>p['hle_candidate_changes']:
                raise ValueError('Impossible scoped-HLE mismatch counts')
            if i in (0,3) and any(p[k] for k in keys):raise ValueError('Baseline contains scoped treatment evidence')
        events=e.get('range_mismatches')
        if not isinstance(events,list) or len(events)>32 or len(events)+integer(e,'range_mismatches_dropped')!=total['hle_candidate_changes']:
            raise ValueError('Scoped-HLE counterexample ledger does not reconcile')
        for event in events:
            if event.get('phase') not in (1,2) or not integer(event,'publication') or integer(event,'resource')>=integer(w,'resources'):
                raise ValueError('Invalid scoped counterexample identity')
            for k in ('previous_hash_hex','current_hash_hex'):
                if len(event[k])!=16:raise ValueError('Invalid scoped counterexample digest')
                int(event[k],16)
            if event['previous_hash_hex']==event['current_hash_hex']:raise ValueError('Equal digests in counterexample')
        if total['hle_candidate_changes']:
            warnings.append('Full Hash disproved scoped-host candidate(s); no Hash skipping is allowed')
        if not total['hle_candidates']:
            warnings.append('No scoped-host candidates: inspect sampled unknown callers and range/history blocks; do not rerun unchanged code')
        if total['hle_candidates'] and not total['submission_candidates']:
            warnings.append('GPU submission epochs exclude all scoped candidates; range ownership/completion integration is still required')
        if not contracts['scoped_calls']:warnings.append('No reviewed output contract actually ran')
    except (KeyError,TypeError,ValueError) as exc:
        issues.append(str(exc))
    work = analyze_candidate_work(e, w, issues)
    blockers = sorted(contracts.get('imports',[]),key=lambda r:r.get('unknown_candidate_checks',0),reverse=True)
    return dict(totals=total,contracts=contracts,unknown_blockers=blockers,candidate_work=work,
                safe_to_skip_hash=False,attribution_sampling_only=64,
                attribution_scope='Full session including restarts; sightings can overlap and are not disjoint shares',
                limits='Known synchronous output ranges only; no GPU completion or asynchronous HLE/IPC authority')


def analyze_candidate_work(e, w, issues):
    """Versioned logical bytes, NOT saved bandwidth/time or skip authority."""
    work = e.get('candidate_work')
    cache = e.get('range_contracts', {}).get('metadata_cache')
    if work is None and cache is None:
        return None  # older evidence has no invented byte estimates
    totals = {k: 0 for k in ('hash_bytes','cpu_bytes','hle_bytes','submission_bytes')}
    try:
        if (not isinstance(work, dict) or type(work.get('version')) is not int or work.get('version') != 1
                or work.get('units') != 'logical_input_bytes_including_palette'):
            raise ValueError('Missing or invalid candidate-work byte contract')
        if (not isinstance(cache, dict) or type(cache.get('version')) is not int or cache.get('version') != 1
                or cache.get('content_reuse_authority') is not False):
            raise ValueError('Invalid metadata-cache contract/authority')
        contracts = e['range_contracts']
        hits = integer(cache, 'nid_hits'); probes = integer(cache, 'nid_probes')
        calls = integer(contracts, 'calls')
        if hits > calls or probes < calls - hits or not integer(cache, 'interests_revision'):
            raise ValueError('NID cache accounting does not reconcile')
        touches = integer(cache, 'no_overlap_hits') + integer(cache, 'range_scans')
        outputs = contracts.get('output_leases')
        extra = 0
        if outputs is not None:
            if (type(outputs.get('version')) is not int or outputs.get('version') not in (1, 2)
                    or outputs.get('authority') is not False):
                raise ValueError('Invalid concrete-output contract/authority')
            starts = integer(outputs, 'begins'); ends = integer(outputs, 'ends')
            active = integer(outputs, 'active')
            if starts != ends + active or active:
                raise ValueError('Concrete output still active or unbalanced at export')
            # v1: generic/ATRAC9/AAC/MP3; v2 appends concrete GXM uniform copies.
            expected_sources = 4 if outputs['version'] == 1 else 5
            source_counts = outputs.get('sources')
            if (not isinstance(source_counts, list) or len(source_counts) != expected_sources
                    or any(type(v) is not int or v < 0 for v in source_counts)
                    or sum(source_counts) != starts):
                raise ValueError('Concrete-output source accounting does not reconcile')
            if integer(outputs, 'capacity_bytes') < starts:
                raise ValueError('Concrete output has impossible byte-capacity count')
            integer(outputs, 'overlap_touches')
            extra = starts + ends
            for row in contracts.get('imports', []):
                if integer(row, 'unknown_finishes') + integer(row, 'scoped') != integer(row, 'calls'):
                    raise ValueError('Root unknown/scoped completion ledger does not reconcile')
                if integer(row, 'unknown_prefixes') > row['scoped']:
                    raise ValueError('More unknown prefixes than declared root calls')
        if integer(e, 'active_host_at_export') or touches != 2 * integer(contracts, 'scoped_calls') + extra:
            raise ValueError('Output intersection cache begin/end accounting does not reconcile')
        phases = work.get('phases')
        if not isinstance(phases, list) or len(phases) != 4:
            raise ValueError('Candidate byte ledger requires four phases')
        for i, row in enumerate(phases):
            if row.get('index') != i: raise ValueError('Candidate byte phase identity mismatch')
            values = [integer(row,k) for k in totals]
            if not values[0] >= values[1] >= values[2] >= values[3]:
                raise ValueError('Candidate byte subsets do not reconcile')
            if values[0] != integer(w['phases'][i], 'hash_bytes'):
                raise ValueError('Full Hash byte ledger mismatch')
            counts = (integer(e['phases'][i], 'cpu_candidates'), integer(e['range_phases'][i], 'hle_candidates'),
                      integer(e['range_phases'][i], 'submission_candidates'))
            for count, value in zip(counts, values[1:]):
                if value < count or bool(count) != bool(value):
                    raise ValueError('Candidate calls/bytes disagree')
            if i in (1,2):
                for k in totals: totals[k] += row[k]
    except (KeyError, IndexError, TypeError, ValueError) as exc:
        issues.append(str(exc))
    return dict(treatment_totals=totals, metadata_cache=cache,
                output_leases=e.get('range_contracts', {}).get('output_leases'),
                includes_settling=True, safe_to_skip_hash=False,
                limits='Repeated logical input including palette; not unique bytes, DRAM traffic or measured savings')

def analyze_callback_polls(e, issues):
    """Optional versioned extension; legacy captures do not invent callback proof."""
    report = e.get('range_contracts', {}).get('callback_polls')
    if report is None:
        return None
    fields = ('polls','empty','nonempty','incomplete','reported_callbacks',
              'uses','clean_uses','conservative_uses','abandoned')
    try:
        if (not isinstance(report, dict) or type(report.get('version')) is not int or report['version'] != 1
                or report.get('authority') is not False or report.get('failed') is not False
                or report.get('event_phase_model') != 'event_time_including_settling'):
            raise ValueError('Unsupported/failed callback-poll contract')
        if integer(report, 'active_phase') != 4:
            raise ValueError('Callback phase was not closed before export')
        integer(report, 'capture_serial')
        total = report['total']; phases = report['phases']
        if not isinstance(phases, list) or len(phases) != 4:
            raise ValueError('Callback poll phases must have four rows')
        for row in [total] + phases:
            for k in fields: integer(row, k)
            if row['polls'] != row['empty'] + row['nonempty'] + row['incomplete']:
                raise ValueError('Callback poll partition mismatch')
            if row['uses'] != row['clean_uses'] + row['conservative_uses']:
                raise ValueError('Callback prefix use partition mismatch')
            if row['reported_callbacks'] < row['nonempty']:
                raise ValueError('Nonempty scan without a reported callback')
        # A poll may finish in one phase and its proof be used in the next.
        if total['clean_uses'] > total['empty']:
            raise ValueError('More clean prefix uses than complete empty scans')
        for k in fields:
            if sum(row[k] for row in phases) > total[k]:
                raise ValueError('Phase callback events exceed session total')
        if total['abandoned']:
            issues.append('Callback scan abandoned; inspect unwinding before using capture')
    except (KeyError, TypeError, ValueError) as exc:
        issues.append(str(exc))
    return report
