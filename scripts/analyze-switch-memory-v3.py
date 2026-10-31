#!/usr/bin/env python3
"""Interpret live first-write observations without claiming cache correctness.

Standard library only. Both A and B retain full Hash and use the same native
write-table code. Counters include settling; timing excludes settling. No ratio
is returned for incomplete/invalid/unexercised live observations.
"""
import argparse
import html
import json
import math
from pathlib import Path


def number(obj, key, *, integer=False):
    v = obj.get(key)
    if isinstance(v, bool) or not isinstance(v, (int, float)) or not math.isfinite(v) or v < 0:
        raise ValueError(f'invalid {key}')
    if integer and int(v) != v:
        raise ValueError(f'non-integer {key}')
    return v


def analyze(data, cache_changed_total=None):
    tests = data.get('tests', [])
    counts = {s: sum(t.get('status') == s for t in tests) for s in ('pass', 'fail', 'skip')}
    counts['unknown'] = sum(t.get('status') not in counts for t in tests)
    base = dict(validation_revision=data.get('validation_revision'), capture_outcome=data.get('capture_outcome'),
                test_counts=counts, available='write_shadow' in data)
    if 'write_shadow' not in data:
        return dict(**base, note='No live write-shadow section; this is not a failed device experiment.')
    watch = data['write_shadow']; bench = data.get('benchmark', {})
    reasons, warnings = [], []
    if data.get('strict_hash') is not True or data.get('hash_skipping') is not False:
        reasons.append('Full-Hash reference not confirmed')
    persistent = data.get('validation_revision') == 4
    expected_scope = 'JIT_persistent_range_plus_HLE_notices_observational' if persistent else 'JIT_first_write_only_observational'
    if watch.get('hash_skip_authority') is not False or watch.get('scope') != expected_scope or watch.get('schema_version') != (2 if persistent else 1):
        reasons.append('Unrecognized write-observation contract')
    if counts['fail'] or counts['unknown'] or data.get('tests_dropped', 0):
        reasons.append('Failed/unknown/lost startup records')
    if bench.get('experiment') != 'live_write_shadow' or bench.get('order') != 'ABBA':
        reasons.append('Not the four-phase live ABBA experiment')
    if bench.get('completed') is not True or bench.get('aborted') is not False or bench.get('frames_overflow', 0):
        reasons.append('Incomplete/interrupted/overflowed capture')
    if watch.get('bound') is not True or watch.get('failed') is not False:
        reasons.append('Live table not bound or observation failed')
    phases = bench.get('phases', []); observed = watch.get('phases', [])
    if len(phases) != 4 or len(observed) != 4:
        reasons.append('Missing or duplicate phases')
    checked = []
    try:
        target_ms = number(bench, 'measure_seconds') * 1000
        if target_ms <= 0: raise ValueError('empty target duration')
        for i, (p, c) in enumerate(zip(phases, observed)):
            if i >= 4: break
            expected = 'write_watch' if i in (1, 2) else 'baseline'
            if p.get('index') != i or c.get('index') != i or p.get('variant') != expected:
                raise ValueError('wrong phase order / identity')
            n, duration = number(p, 'measured_intervals', integer=True), number(p, 'duration_ms')
            rate = number(p, 'loops_per_second')
            if p.get('complete') is not True or n < 100 or duration < target_ms * .9 or duration <= 0:
                reasons.append(f'Phase {i}: insufficient measured data')
            if duration > 0 and not math.isclose(rate, n * 1000 / duration, rel_tol=1e-5, abs_tol=1e-5):
                reasons.append(f'Phase {i}: rate disagrees with counts/duration')
            for k in ('hash_checks', 'hash_bytes', 'covered_checks', 'changed', 'changed_cpu_seen',
                      'changed_unobserved', 'changed_uncovered', 'callback_stores', 'callback_reads',
                      'exclusive_success', 'invalid_accesses', 'first_write_pages', 'armed_pages',
                      'armed_aliases', 'mapping_updates', 'resources_rejected'):
                number(c, k, integer=True)
            if persistent:
                for key in ('watched_stores', 'exact_resource_hits', 'page_only_stores', 'host_notices',
                            'host_resource_hits', 'changed_host_seen', 'changed_raced', 'discarded_tokens', 'guard_budget_exhausted'):
                    number(c, key, integer=True)
                if c['guard_budget_exhausted']:
                    reasons.append(f'Phase {i}: persistent guard budget exhausted; treatment stopped')
            extra_changes = c.get('changed_host_seen', 0) + c.get('changed_raced', 0) if persistent else 0
            if c['covered_checks'] > c['hash_checks'] or c['changed'] != c['changed_cpu_seen'] + c['changed_unobserved'] + c['changed_uncovered'] + extra_changes:
                reasons.append(f'Phase {i}: inconsistent observation counts')
            if c['invalid_accesses']:
                reasons.append(f'Phase {i}: invalid callback access observed')
            if i in (1, 2) and (not c['armed_pages'] or not c['covered_checks']):
                reasons.append(f'Phase {i}: no exercised watched footprint; no overhead claim')
            checked.append(dict(index=i, variant=expected, loops_per_second=rate, measured_intervals=n,
                                duration_ms=duration, observations=c))
    except (ValueError, TypeError, KeyError) as e:
        reasons.append(str(e))
    if watch.get('capacity_limited') or any(p.get('resources_rejected', 0) for p in observed):
        warnings.append('Bounded footprint capacity reached; observations are partial')
    if watch.get('events_dropped', 0):
        warnings.append('Event timeline truncated; aggregate counters remain separately available')
    unknown = sum(p.get('changed_unobserved', 0) for p in observed)
    if unknown:
        warnings.append('Hash changes without observed JIT writes were recorded. This does NOT attribute them to HLE/GPU: registration races and other untraced paths remain possible.')
    content_changes = sum(p.get('changed', 0) for p in observed)
    if not content_changes:
        warnings.append('No comparable live content mutations were observed. Zero unobserved changes is a vacuous negative result; submit this capture, do not blindly repeat it.')
    ledger = watch.get('cache_ledger')
    ledger_check = dict(available=isinstance(ledger, dict), telemetry_changed_total=cache_changed_total,
                        matched_to_telemetry=None)
    if persistent:
        required = {'native_exact_store', 'native_repeat_alias_store', 'raw_after_cpu_negative_control',
                    'page_neighbor_negative_control', 'host_notice_positive_control', 'observer_CAS_contract',
                    'cache_first_baseline_ledger', 'remap_discards_inflight_hash', 'canary_cleanup'}
        present = [t.get('name') for t in tests if t.get('suite') == 'write_watch_canary' and t.get('status') == 'pass']
        if set(present) != required or len(present) != len(required):
            reasons.append('Missing, duplicated or failed automatic mutation canaries')
        try:
            if not isinstance(ledger, dict): raise ValueError('Missing actual TextureCache comparison ledger')
            n = number(ledger, 'changes', integer=True)
            compares = number(ledger, 'comparisons', integer=True)
            lost = number(ledger, 'events_dropped', integer=True)
            parts = ledger.get('reasons', {})
            keys = {'cpu_interval', 'host_notice_interval', 'unobserved', 'uncovered', 'first_observer_baseline',
                    'different_observer_baseline', 'raced_or_invalidated', 'missing_observation', 'unsupported_hash_path'}
            if set(parts) != keys or sum(number(parts, k, integer=True) for k in keys) != n:
                raise ValueError('Cache-change explanation counters do not partition all changes')
            if n > compares or len(ledger.get('events', [])) + lost != n:
                raise ValueError('Cache-change totals disagree with stored/lost event counts')
            if cache_changed_total is not None:
                ledger_check['matched_to_telemetry'] = n == cache_changed_total
                if n != cache_changed_total:
                    reasons.append('Cache-change ledger disagrees with session TextureHashChanged telemetry')
            else:
                warnings.append('Session TextureHashChanged total unavailable; ledger reconciliation not verified')
            if lost: warnings.append('Cache-change event timeline truncated; reason counters retain all changes')
            ledger_check.update(changes=n, comparisons=compares, reasons=parts, events_dropped=lost)
        except (ValueError, TypeError, KeyError) as e:
            reasons.append(str(e))
    elif cache_changed_total is not None and cache_changed_total > content_changes:
        warnings.append('Session TextureCache changes exceed comparable observer changes. Different time windows/baselines are not reconciled by v3; a zero observer count is not zero cache changes.')
    pairs = []
    if not reasons and len(checked) == 4:
        for a, b in ((0, 1), (3, 2)):
            ar, br = checked[a]['loops_per_second'], checked[b]['loops_per_second']
            if ar <= 0 or br <= 0:
                reasons.append('Zero throughput'); break
            pairs.append(dict(baseline_phase=a, observation_phase=b,
                              throughput_delta_pct=(br/ar - 1)*100,
                              loop_time_delta_pct=(ar/br - 1)*100))
    if reasons: pairs = []
    return dict(**base, comparison_eligible=not reasons, exclusions=reasons, warnings=warnings,
                phases=checked, pairs=pairs, changed_without_observed_cpu=unknown,
                footprint_count=watch.get('resources'), backing_pages=watch.get('backing_pages'),
                events=watch.get('events', []), footprints=watch.get('footprints', []),
                comparable_content_changes=content_changes, cache_ledger_check=ledger_check,
                actual_cache_events=ledger.get('events', []) if isinstance(ledger, dict) else [],
                interpretation=[
                    'This measures observation overhead, NOT a hashless speedup. Both variants retain full Hash.',
                    'A already includes the mirror table, boundary checks and footprint lookups. It is NOT the old uninstrumented baseline.',
                    ('CPU/host notices refer to exact ranges since the previous comparable Hash, not a proven exclusive writer; metadata stability does not prove memory stability.' if persistent else 'CPU-seen is sticky within a B epoch; it does not identify the writer of each individual content change.'),
                    'Covered means software-alias/host-page dependencies were armed; it is NOT complete CPU/HLE/GPU coverage.',
                    'Arming is observational and has no all-JIT safepoint. Zero unexplained changes is NOT a correctness proof.',
                    'Timing excludes settle/boundary intervals; observation counters include the entire phase.',
                    'New B-only footprints are explicitly uncovered until the next B epoch.',
                ])


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('session', type=Path); p.add_argument('--html', type=Path)
    args = p.parse_args()
    path = args.session/'memory-validation.json' if args.session.is_dir() else args.session
    try:
        total = None
        summary_path = path.parent/'summary.json'
        if summary_path.exists():
            summary = json.loads(summary_path.read_text(encoding='utf-8'))
            matches = [m for m in summary.get('metrics', []) if m.get('metric') == 'texture_hash_changed']
            if len(matches) == 1: total = number(matches[0], 'calls', integer=True)
        report = analyze(json.loads(path.read_text(encoding='utf-8')), total)
        result = json.dumps(report, indent=2, ensure_ascii=False, allow_nan=False)
    except (OSError, ValueError, TypeError) as e:
        p.exit(2, str(e)+'\n')
    if args.html:
        text = '<!doctype html><meta charset="utf-8"><title>Vita3K 写入观察报告</title>'
        text += '<style>body{font:16px system-ui;max-width:1100px;margin:40px auto;padding:0 24px}pre{white-space:pre-wrap;overflow-wrap:anywhere;background:#f3f4f6;padding:20px}</style>'
        text += '<h1>Vita3K 实时写入观察</h1><p>完整 Hash 保留。没有未归因变化不等于已证明可以跳过 Hash；比较值表示观察成本，不表示免 Hash 提速。</p>'
        text += '<pre>'+html.escape(result)+'</pre>'
        args.html.write_text(text, encoding='utf-8')
    print(result)


if __name__ == '__main__': main()
