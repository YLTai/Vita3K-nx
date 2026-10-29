#!/usr/bin/env python3
"""One entry point for the compact capture. Accept a session directory or ZIP.

No third-party packages, file extraction, or fixed-N Hash skip conclusions.
Time-symmetric A/B/C/C/B/A samples are descriptive, not a significance test.
"""
import argparse
import html
import json
import math
from pathlib import Path
import zipfile

BASE = '4101bf2cf13a21fdac1792caf26e34324166398b'
ORDER = ['baseline', 'first_write', 'exact_range', 'exact_range', 'first_write', 'baseline']
CONTROLS = {
    'first_write_reopens_aliases', 'open_page_repeat_negative_control',
    'native_exact_store', 'native_repeat_alias_store', 'native_same_value_store',
    'raw_after_cpu_negative_control', 'page_neighbor_negative_control',
    'host_notice_positive_control', 'observer_CAS_contract',
    'cache_first_baseline_ledger', 'cache_shared_history_ledger',
    'remap_discards_inflight_hash', 'native_palette_only_store',
    'native_crosspage_exact_ranges', 'canary_cleanup',
}
REASONS = {'cpu_interval', 'host_notice_interval', 'unobserved', 'uncovered',
           'first_observer_baseline', 'different_observer_baseline',
           'raced_or_invalidated', 'missing_observation', 'unsupported_hash_path'}


def number(obj, key, integer=False):
    value = obj[key]
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value) or value < 0:
        raise ValueError(f'Invalid {key}')
    if integer and (not isinstance(value, int)):
        raise ValueError(f'Non-integer {key}')
    return value


def analyze(data, changed_total=None):
    reasons, warnings = [], []
    bench, watch = data.get('benchmark', {}), data.get('write_shadow', {})
    tests = data.get('tests', [])
    control_rows = [t for t in tests if t.get('suite') == 'write_watch_canary']
    names = [t.get('name') for t in control_rows if t.get('status') == 'pass']
    controls_ok = len(names) == len(CONTROLS) and set(names) == CONTROLS and len(control_rows) == len(CONTROLS)
    if not controls_ok or data.get('controls_complete') is not True:
        reasons.append('Required automatic controls missing, duplicated or failed')
    if any(t.get('status') not in ('pass', 'skip') for t in tests) or data.get('tests_dropped') != 0:
        reasons.append('Startup failure or lost test records')
    if data.get('capture_plan') != 'compact-1' or data.get('baseline_commit') != BASE:
        reasons.append('Wrong capture plan / hardware baseline')
    if data.get('strict_hash') is not True or data.get('hash_skipping') is not False:
        reasons.append('Full-Hash reference is not enabled')
    if data.get('strict_reference_scope') != 'capture_only':
        reasons.append('Unexpected reference scope')
    if bench.get('order') != 'ABCCBA' or bench.get('experiment') != 'compact_write_audit':
        reasons.append('Wrong experiment / phase order')
    if bench.get('completed') is not True or bench.get('aborted') is not False or bench.get('frames_overflow') != 0:
        reasons.append('Incomplete, interrupted or overflowing capture')
    if watch.get('schema_version') != 2 or watch.get('bound') is not True or watch.get('failed') is not False:
        reasons.append('Observer unavailable / failed / unknown schema')
    if watch.get('hash_skip_authority') is not False or watch.get('arm_safepoint') is not False:
        reasons.append('Unexpected observer contract')
    phases, observations = bench.get('phases', []), watch.get('phases', [])
    if len(phases) != 6 or len(observations) != 6:
        reasons.append('Expected exactly six measured and observation phases')
    checked = []
    phase_issues = {i: [] for i in range(6)}
    try:
        if number(bench, 'settle_seconds', True) != 2 or number(bench, 'measure_seconds', True) != 10:
            reasons.append('Unexpected compact timing')
        for i, (p, c) in enumerate(zip(phases, observations)):
            if i >= len(ORDER) or p.get('index') != i or c.get('index') != i or p.get('variant') != ORDER[i]:
                raise ValueError('Wrong phase identity')
            n = number(p, 'measured_intervals', True)
            ms = number(p, 'duration_ms')
            rate = number(p, 'loops_per_second')
            if p.get('complete') is not True or n < 50 or not 9000 <= ms <= 10000 or rate <= 0:
                phase_issues[i].append('insufficient valid measurement')
            if ms and not math.isclose(rate, 1000 * n / ms, rel_tol=1e-5, abs_tol=1e-5):
                phase_issues[i].append('throughput disagrees with interval counts')
            keys = ('changed', 'changed_cpu_seen', 'changed_host_seen', 'changed_unobserved',
                    'changed_uncovered', 'changed_raced', 'hash_checks', 'covered_checks',
                    'armed_pages', 'invalid_accesses', 'guard_budget_exhausted', 'watched_stores',
                    'exact_resource_hits', 'page_only_stores', 'host_notices', 'resources_rejected')
            for k in keys:
                number(c, k, True)
            if c['changed'] != sum(c[k] for k in keys[1:6]) or c['covered_checks'] > c['hash_checks']:
                reasons.append(f'Phase {i}: inconsistent change / coverage counts')
            if c['invalid_accesses']:
                reasons.append(f'Phase {i}: invalid callback access')
            if c['guard_budget_exhausted']:
                phase_issues[i].append('callback-cost limit reached; this treatment was disabled')
            if ORDER[i] != 'baseline' and (not c['armed_pages'] or not c['covered_checks']):
                phase_issues[i].append('no exercised watched footprint')
            if c['resources_rejected']:
                warnings.append(f'Phase {i}: footprint capacity reached; coverage is partial')
            checked.append(dict(index=i, mode=ORDER[i], loops_per_second=rate,
                                measured_intervals=n, duration_ms=ms, observations=c))
    except (KeyError, ValueError, TypeError) as e:
        reasons.append(str(e))
    ledger = watch.get('cache_ledger', {})
    ledger_ok = False
    try:
        changes, compares, lost = (number(ledger, k, True) for k in ('changes', 'comparisons', 'events_dropped'))
        parts = ledger.get('reasons', {})
        if set(parts) != REASONS or sum(number(parts, k, True) for k in REASONS) != changes:
            raise ValueError('Cache changes not partitioned by reasons')
        if changes > compares or len(ledger.get('events', [])) + lost != changes:
            raise ValueError('Stored and lost cache events do not reconcile')
        if changed_total is None:
            reasons.append('summary.json TextureHashChanged count unavailable: reconciliation cannot pass')
        elif not isinstance(changed_total, int) or isinstance(changed_total, bool) or changes != changed_total:
            reasons.append('Cache ledger differs from actual TextureHashChanged count')
        else:
            ledger_ok = True
        if lost:
            warnings.append('Detailed cache-event storage full; aggregate reason counts are preserved')
    except (KeyError, ValueError, TypeError) as e:
        reasons.append(str(e))
    unknown = sum(c.get('changed_unobserved', 0) for c in observations)
    if unknown:
        warnings.append('Unattributed changes are evidence, not a reason to repeat; HLE/GPU and registration races remain possible')
    if not sum(c.get('changed', 0) for c in observations):
        warnings.append('No natural comparable changes. Automatic controls still exercise the mechanisms; do not rerun just for green')
    groups, comparisons = {}, {}
    if not reasons and len(checked) == 6:
        for mode in dict.fromkeys(ORDER):
            samples = [p for p in checked if p['mode'] == mode]
            if any(phase_issues[p['index']] for p in samples):
                continue
            total_n = sum(p['measured_intervals'] for p in samples)
            total_ms = sum(p['duration_ms'] for p in samples)
            groups[mode] = dict(loops_per_second=1000 * total_n / total_ms,
                                mean_loop_ms=total_ms / total_n,
                                phase_rates=[p['loops_per_second'] for p in samples])
        a = groups.get('baseline')
        if a is None:
            reasons.append('Baseline samples unusable; no timing comparison')
        drift = (a['phase_rates'][1] / a['phase_rates'][0] - 1) * 100 if a else 0
        if abs(drift) > 5:
            warnings.append('Baseline endpoints differ by more than 5%; timing is workload-drift-contaminated')
        for mode in ('first_write', 'exact_range'):
            if not a or mode not in groups:
                continue
            b = groups[mode]
            comparisons[mode] = dict(throughput_delta_pct=(b['loops_per_second']/a['loops_per_second']-1)*100,
                extra_ms_per_loop=b['mean_loop_ms']-a['mean_loop_ms'], baseline_drift_pct=drift,
                descriptive_only=True, baseline_drift_warning=abs(drift)>5)
    return dict(plan='compact-1', baseline_commit=BASE, controls_ok=controls_ok,
        cache_ledger_ok=ledger_ok, comparison_eligible=not reasons and not any(phase_issues.values()),
        exclusions=reasons, phase_exclusions=phase_issues, usable_comparisons=list(comparisons),
        warnings=warnings, groups=groups, comparisons=comparisons, phases=checked,
        cache_ledger=ledger, changed_without_observed_writer=unknown,
        next_action='Share this capture; do not repeat merely to obtain a green result',
        limits=['Full Hash remains authoritative: this is not a hashless speedup.',
                'A shares the write table, boundary checks and footprint registry; not total overhead vs the original JIT.',
                'B observes first writes; C keeps selected pages guarded and consumes exact evidence per comparison.',
                'No all-JIT rearm safepoint; host notices are post-write and incomplete. Version stability is not memory stability.',
                'Two time-symmetric samples per mode do not prove statistical significance or graphics correctness.'])


def load_session(path):
    def total(summary):
        rows = [m for m in summary.get('metrics', []) if m.get('metric') == 'texture_hash_changed']
        return number(rows[0], 'calls', True) if len(rows) == 1 else None
    if path.is_dir():
        data = json.loads((path/'memory-validation.json').read_text(encoding='utf-8'))
        summary = path/'summary.json'
        return data, total(json.loads(summary.read_text(encoding='utf-8'))) if summary.exists() else None
    with zipfile.ZipFile(path) as archive:
        names = [n for n in archive.namelist() if n.endswith('/memory-validation.json') or n == 'memory-validation.json']
        if len(names) != 1:
            raise ValueError('ZIP must contain exactly one session')
        name = names[0]
        def read(member):
            info = archive.getinfo(member)
            if info.file_size > 32 * 1024 * 1024:
                raise ValueError('Oversized JSON report')
            return json.loads(archive.read(member))
        summary_name = name[:-len('memory-validation.json')] + 'summary.json'
        return read(name), total(read(summary_name)) if summary_name in archive.namelist() else None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('session', type=Path)
    parser.add_argument('--html', type=Path)
    args = parser.parse_args()
    try:
        report = analyze(*load_session(args.session))
        text = json.dumps(report, indent=2, ensure_ascii=False, allow_nan=False)
        if args.html:
            args.html.write_text('<!doctype html><meta charset="utf-8"><title>Vita3K compact capture</title>'
                '<style>body{font:16px system-ui;max-width:1100px;margin:36px auto;padding:0 24px}'
                'pre{white-space:pre-wrap;overflow-wrap:anywhere}</style>'
                '<h1>一次采集：A/B/C/C/B/A</h1><p>自动反例通过不等于完整游戏写入覆盖。性能比较是观察开销，不是免 Hash 提速。</p>'
                '<pre>'+html.escape(text)+'</pre>', encoding='utf-8')
        print(text)
    except (OSError, ValueError, KeyError, TypeError, zipfile.BadZipFile) as error:
        parser.exit(2, str(error)+'\n')


if __name__ == '__main__':
    main()
