#!/usr/bin/env python3
"""Analyze the v2 same-JIT store matrix; standard library only.

Accepts a TraceLab session folder or its memory-validation.json. This augments,
not replaces, analyze-switch-tracelab.mjs (gameplay timing and staging A/B).
Unknown, duplicate, failed, migrated, and incomplete pairs never yield a ratio.
CPU ticks are a query-inclusive bracket, not pure store cycles or CPU frequency.
"""
import argparse
import json
import math
import statistics
import sys
from pathlib import Path

MODES = ('original_table', 'same_table_x25', 'mirror_write_table',
         'first_write_table', 'preopened_write_table')
COMPARISONS = (
    ('original_table', 'same_table_x25'),
    ('same_table_x25', 'mirror_write_table'),
    ('mirror_write_table', 'first_write_table'),
    ('mirror_write_table', 'preopened_write_table'),
)


def detail_fields(text):
    return dict(part.split('=', 1) for part in text.split() if '=' in part)


def integer(fields, key):
    value = fields[key]
    if isinstance(value, bool):
        raise ValueError(f'{key}: boolean is not a sample count')
    if isinstance(value, int):
        return value
    if not isinstance(value, str):
        raise ValueError(f'{key}: not an integer')
    return int(value, 0)


def analyze(report):
    hz = report.get('timebase_hz')
    if isinstance(hz, bool) or not isinstance(hz, (int, float)) or not math.isfinite(hz) or hz <= 0:
        raise ValueError('timebase_hz must be positive and finite')
    samples, excluded, seen = {}, [], set()
    matrix_count = 0
    for record in report.get('tests', []):
        if record.get('suite') != 'store_matrix_v2' or record.get('name') == 'scope':
            continue
        matrix_count += 1
        fields = detail_fields(record.get('detail', ''))
        name = record.get('name', '?')
        key = None
        try:
            mode = fields['mode']
            b, layout, row = (integer(fields, k) for k in ('boundary_checks', 'reverse_layout', 'round'))
            key = (b, layout, row, mode)
            if mode not in MODES or b not in (0, 1) or layout not in (0, 1) or not 0 <= row < 5:
                raise ValueError('unknown mode / matrix coordinates')
            if key in seen:
                samples.pop(key, None)
                raise ValueError('duplicate coordinate: neither sample is usable')
            seen.add(key)
            if record.get('status') != 'pass':
                raise ValueError('device correctness check did not pass')
            elapsed, stores = integer(record, 'elapsed_ticks'), integer(record, 'iterations')
            if elapsed <= 0 or stores != 262144:
                raise ValueError('invalid timer or unexpected store count')
            core_before, core_after = integer(fields, 'core_before'), integer(fields, 'core_after')
            if core_before < 0 or core_before != core_after:
                raise ValueError('migration detected at core endpoints')
            expected = 4 if mode == 'first_write_table' else 0
            if integer(fields, 'callbacks') != expected or integer(fields, 'expected_callbacks') != expected:
                raise ValueError('callback count is inconsistent with this mode')
            if integer(fields, 'compile_outside_timing') != 1 or integer(fields, 'shared_split_jit') != 1:
                raise ValueError('missing warmup / shared-code contract')
            expected_jit = 0 if mode == MODES[0] else 1 if mode == MODES[1] else 2
            if integer(fields, 'jit_slot') != expected_jit:
                raise ValueError('wrong JIT slot')
            identity = {k: integer(fields, k) for k in ('read_table', 'write_table', 'backing', 'jit_object', 'guest_pc')}
            if any(identity[k] == 0 for k in ('read_table', 'backing', 'jit_object', 'guest_pc')):
                raise ValueError('missing address identity')
            if mode == MODES[0] and identity['write_table'] != 0:
                raise ValueError('original sample has a write table')
            if mode == MODES[1] and identity['write_table'] != identity['read_table']:
                raise ValueError('same-table X25 does not point to the read table')
            if mode in MODES[2:] and (not identity['write_table'] or identity['write_table'] == identity['read_table']):
                raise ValueError('split-table sample does not use an independent table')
            cpu_valid = integer(fields, 'cpu_ticks_valid') == 1
            cpu = integer(fields, 'cpu_ticks')
            bracket = integer(fields, 'bracket_ticks')
            if bracket < elapsed:
                raise ValueError('CPU-query bracket is shorter than the inner wall timer')
            # Invalid/unavailable CPU ticks do not suppress independently valid
            # wall data; never fabricate a CPU ratio in this case.
            cpu_valid = (cpu_valid and integer(fields, 'cpu_rc_before') == 0
                         and integer(fields, 'cpu_rc_after') == 0 and 0 < cpu <= bracket)
            samples[key] = dict(name=name, mode=mode, core=core_before, **identity,
                                ns_per_store=elapsed / hz * 1e9 / stores,
                                cpu_ns_per_store=cpu / hz * 1e9 / stores if cpu_valid else None)
        except (KeyError, TypeError, ValueError, OverflowError) as error:
            excluded.append(dict(name=name, reason=str(error)))

    comparisons = []
    dropped = report.get('tests_dropped', 0) != 0
    for b in (0, 1):
        for layout in (0, 1):
            for left, right in COMPARISONS:
                wall_ratios, cpu_ratios, reasons = [], [], []
                for row in range(5):
                    a, z = samples.get((b, layout, row, left)), samples.get((b, layout, row, right))
                    reason = None
                    if dropped:
                        reason = 'test records were dropped'
                    elif not a or not z:
                        reason = 'missing or rejected sample'
                    elif a['core'] != z['core']:
                        reason = 'pair executed on different endpoint cores'
                    elif any(a[k] != z[k] for k in ('read_table', 'backing', 'guest_pc')):
                        reason = 'pair did not use the same fixture'
                    elif left in MODES[2:] and (a['jit_object'] != z['jit_object'] or a['write_table'] != z['write_table']):
                        reason = 'split states did not share the same JIT and table'
                    if reason:
                        reasons.append(dict(round=row, reason=reason))
                        continue
                    wall_ratios.append(z['ns_per_store'] / a['ns_per_store'])
                    if a['cpu_ns_per_store'] is not None and z['cpu_ns_per_store'] is not None:
                        cpu_ratios.append(z['cpu_ns_per_store'] / a['cpu_ns_per_store'])
                # All five Latin rows are required: no favorable partial result.
                complete = len(wall_ratios) == 5
                comparisons.append(dict(
                    boundary_checks=b, reverse_layout=layout, baseline=left, treatment=right,
                    complete=complete, paired_rows=len(wall_ratios), excluded_pairs=reasons,
                    wall_ratio_median=statistics.median(wall_ratios) if complete else None,
                    wall_ratio_min=min(wall_ratios) if complete else None,
                    wall_ratio_max=max(wall_ratios) if complete else None,
                    cpu_paired_rows=len(cpu_ratios),
                    cpu_bracket_ratio_median=statistics.median(cpu_ratios) if complete and len(cpu_ratios) == 5 else None,
                ))
    return dict(
        schema_version=1, source_validation_revision=report.get('validation_revision', 1),
        capture_outcome=report.get('capture_outcome', 'not_recorded'),
        matrix_available=matrix_count > 0, expected_samples=100, observed_samples=matrix_count,
        accepted_samples=len(samples), excluded_samples=excluded,
        comparisons=comparisons if matrix_count else [],
        caveats=[
            'Host analysis is not verification of native A64 execution or GPU correctness.',
            'ns/store includes the Guest loop and JIT dispatch; it is not bare STR latency.',
            'CPU brackets include two query calls. CPU ticks use timebase_hz, not CPU frequency.',
            'Equal endpoint cores cannot rule out intermediate migration or preemption.',
            'Do not pool boundary-check or allocation-order groups or compare to a different NRO.',
            'No ratio is published unless all five pairs in that group are usable.',
        ],
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('session', type=Path)
    args = parser.parse_args()
    path = args.session / 'memory-validation.json' if args.session.is_dir() else args.session
    try:
        report = analyze(json.loads(path.read_text(encoding='utf-8')))
    except (OSError, ValueError, TypeError) as error:
        parser.exit(2, f'{path}: {error}\n')
    json.dump(report, sys.stdout, indent=2, ensure_ascii=False, allow_nan=False)
    sys.stdout.write('\n')


if __name__ == '__main__':
    main()
