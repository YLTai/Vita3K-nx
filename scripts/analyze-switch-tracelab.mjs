#!/usr/bin/env node
// Vita3K Switch TraceLab session reader. No npm dependencies.
// SPDX-License-Identifier: GPL-2.0-or-later
import { readFile, writeFile } from 'node:fs/promises';
import { join } from 'node:path';

const usage = `Usage: node scripts/analyze-switch-tracelab.mjs <session-directory> [--start-ms N] [--end-ms N] [--html report.html] [--trace-json events.json]`;

function csv(text) {
    const lines = text.trim().split(/\r?\n/);
    const names = lines.shift().split(',');
    return lines.filter(Boolean).map(line =>
        Object.fromEntries(line.split(',').map((part, i) => [names[i], part])));
}

function quantile(values, p) {
    if (!values.length) return null;
    const sorted = [...values].sort((a, b) => a - b);
    const index = (sorted.length - 1) * p;
    const lo = Math.floor(index), hi = Math.ceil(index);
    return sorted[lo] + (sorted[hi] - sorted[lo]) * (index - lo);
}

function isFiniteNumber(s) {
    return Number.isFinite(Number(s)) && String(s).trim().length;
}
function escapeHtml(s) {
    return String(s).replace(/[&<>"']/g, c =>
        ({ '&':'&amp;', '<':'&lt;', '>':'&gt;', '"':'&quot;', "'":'&#39;' })[c]);
}

const MEMORY_MIN_INTERVALS = 100;
const MEMORY_REFERENCE_SCOPE = 'ordinary_texture_cache_existing_hash_footprint';
const MEMORY_PHASE_ORDER = ['baseline', 'dedup', 'dedup', 'baseline',
    'dedup', 'baseline', 'baseline', 'dedup'];

function nonnegativeNumber(value) {
    return typeof value === 'number' && Number.isFinite(value) && value >= 0
        ? value : null;
}

function testCounts(tests) {
    const counts = {pass: 0, fail: 0, skip: 0, unknown: 0};
    for (const test of tests)
        ++counts[Object.hasOwn(counts, test.status) ? test.status : 'unknown'];
    return counts;
}

async function readMemoryValidation(dir, manifest) {
    let source;
    try {
        source = await readFile(join(dir, 'memory-validation.json'), 'utf8');
    } catch (error) {
        if (error.code === 'ENOENT')
            return {available: false, explanation: 'This capture has no memory-validation.json.'};
        throw error;
    }
    let data;
    try {
        data = JSON.parse(source);
    } catch (error) {
        return {available: true, schema_supported: false,
            explanation: `Invalid memory-validation.json: ${error.message}`};
    }
    if (!data || data.schema_version !== 1 || !Array.isArray(data.tests)
        || data.tests.some(test => !test || typeof test !== 'object')
        || (data.benchmark?.phases !== undefined
            && (!Array.isArray(data.benchmark.phases)
                || data.benchmark.phases.some(phase => !phase || typeof phase !== 'object'))))
        return {available: true, schema_supported: false,
            explanation: 'Unsupported or incomplete memory validation schema; no performance comparison.'};

    const hz = nonnegativeNumber(data.timebase_hz);
    const testsDropped = Number.isSafeInteger(data.tests_dropped) && data.tests_dropped >= 0
        ? data.tests_dropped : null;
    const tests = data.tests.map(test => {
        const suite = String(test.suite ?? 'unknown');
        const category = suite === 'capability' ? 'capabilities'
            : suite === 'store_microbench' ? 'store_microbenchmarks'
            : suite === 'selftest' || suite.endsWith('_selftest') ? 'self_tests' : 'other';
        const elapsed = nonnegativeNumber(test.elapsed_ticks);
        const iterations = nonnegativeNumber(test.iterations);
        return {
            suite, category, name: String(test.name ?? ''),
            status: ['pass', 'fail', 'skip'].includes(test.status) ? test.status : 'unknown',
            detail: String(test.detail ?? ''), elapsed_ticks: elapsed, iterations,
            // Only this suite defines an iteration as one store. Arbitrary
            // correctness tests and capability probes must not acquire ns/store.
            ns_per_store: category === 'store_microbenchmarks' && test.status === 'pass'
                && hz > 0 && elapsed > 0 && iterations > 0
                ? elapsed * 1e9 / hz / iterations : null
        };
    });
    const groups = Object.fromEntries(['self_tests', 'capabilities', 'store_microbenchmarks', 'other']
        .map(category => [category, testCounts(tests.filter(test => test.category === category))]));
    const benchmark = data.benchmark ?? {};
    const measureSeconds = nonnegativeNumber(benchmark.measure_seconds);
    const minDuration = measureSeconds > 0 ? measureSeconds * 1000 * 0.9 : null;
    const phases = (Array.isArray(benchmark.phases) ? benchmark.phases : []).map(phase => {
        const result = {
            index: phase.index, variant: phase.variant, complete: phase.complete === true,
            measured_intervals: nonnegativeNumber(phase.measured_intervals),
            duration_ms: nonnegativeNumber(phase.duration_ms),
            loops_per_second: nonnegativeNumber(phase.loops_per_second),
            p50_ms: nonnegativeNumber(phase.p50_ms),
            p95_ms: nonnegativeNumber(phase.p95_ms),
            p99_ms: nonnegativeNumber(phase.p99_ms),
            publish_calls: nonnegativeNumber(phase.publish_calls),
            publish_bytes: nonnegativeNumber(phase.publish_bytes),
            dedup_calls: nonnegativeNumber(phase.dedup_calls),
            dedup_bytes: nonnegativeNumber(phase.dedup_bytes),
            excluded_intervals: nonnegativeNumber(phase.excluded_intervals),
            excluded_duration_ms: nonnegativeNumber(phase.excluded_duration_ms)
        };
        const reasons = [];
        if (!result.complete) reasons.push('phase_incomplete');
        if (!Number.isInteger(result.measured_intervals)
            || result.measured_intervals < MEMORY_MIN_INTERVALS)
            reasons.push('too_few_intervals');
        if (minDuration === null || result.duration_ms === null || result.duration_ms < minDuration)
            reasons.push('measurement_too_short');
        if (!(result.loops_per_second > 0) || !(result.p50_ms > 0)
            || !(result.p95_ms >= result.p50_ms) || !(result.p99_ms >= result.p95_ms))
            reasons.push('invalid_cadence');
        if ([result.publish_calls, result.publish_bytes, result.dedup_calls, result.dedup_bytes]
            .some(value => !Number.isSafeInteger(value) || value < 0)
            || result.dedup_calls > result.publish_calls || result.dedup_bytes > result.publish_bytes)
            reasons.push('invalid_publish_counters');
        if (result.variant === 'baseline' && (result.dedup_calls || result.dedup_bytes))
            reasons.push('baseline_contains_dedup');
        return {...result, comparison_exclusions: reasons};
    });
    const exclusions = [];
    if (data.enabled !== true) exclusions.push('validation_disabled');
    if (data.strict_hash !== true || data.hash_skipping !== false)
        exclusions.push('strict_hash_reference_not_confirmed');
    if (data.reference_scope !== MEMORY_REFERENCE_SCOPE)
        exclusions.push('reference_scope_not_confirmed');
    if (data.title !== manifest.title) exclusions.push('title_mismatch');
    if (benchmark.requested !== true) exclusions.push('benchmark_not_requested');
    if (benchmark.completed !== true) exclusions.push('benchmark_incomplete');
    if (benchmark.aborted !== false) exclusions.push('benchmark_aborted_or_unknown');
    if (benchmark.frames_overflow !== false && benchmark.frames_overflow !== 0)
        exclusions.push('frame_overflow_or_unknown');
    if (phases.length !== MEMORY_PHASE_ORDER.length
        || phases.some((phase, index) => phase.index !== index || phase.variant !== MEMORY_PHASE_ORDER[index]))
        exclusions.push('incomplete_or_unexpected_phase_order');
    if (phases.some(phase => phase.comparison_exclusions.length))
        exclusions.push('phase_quality_check_failed');
    if (groups.self_tests.fail || groups.self_tests.unknown)
        exclusions.push('self_test_failed_or_unknown');
    if (testsDropped === null) exclusions.push('test_capture_completeness_not_reported');
    else if (testsDropped > 0) exclusions.push('test_results_incomplete');

    // Adjacent AB / BA pairs keep comparison local in time. Require the entire
    // eight-phase run to be complete; a truncated run is never a claimed gain.
    const delta = (baseline, dedup) => baseline > 0 && dedup !== null
        ? 100 * (dedup / baseline - 1) : null;
    const pairs = [];
    for (let offset = 0; offset + 1 < phases.length; offset += 2) {
        const members = phases.slice(offset, offset + 2);
        const baseline = members.find(phase => phase.variant === 'baseline');
        const dedup = members.find(phase => phase.variant === 'dedup');
        const reasons = [...exclusions];
        if (!baseline || !dedup) reasons.push('pair_missing_variant');
        if (dedup && !(dedup.dedup_calls > 0 && dedup.dedup_bytes > 0))
            reasons.push('no_actual_staging_dedup');
        const eligible = reasons.length === 0;
        pairs.push({
            pair: pairs.length, baseline_phase: baseline?.index ?? null,
            dedup_phase: dedup?.index ?? null, eligible,
            comparison_exclusions: reasons,
            loops_per_second_delta_pct: eligible ? delta(baseline.loops_per_second, dedup.loops_per_second) : null,
            p50_interval_delta_pct: eligible ? delta(baseline.p50_ms, dedup.p50_ms) : null,
            p95_interval_delta_pct: eligible ? delta(baseline.p95_ms, dedup.p95_ms) : null,
            p99_interval_delta_pct: eligible ? delta(baseline.p99_ms, dedup.p99_ms) : null
        });
    }
    const allPairsEligible = pairs.length === 4 && pairs.every(pair => pair.eligible);
    const loopDeltas = pairs.map(pair => pair.loops_per_second_delta_pct);
    return {
        available: true, schema_supported: true, schema_version: data.schema_version,
        title: data.title, enabled: data.enabled === true, strict_hash: data.strict_hash === true,
        hash_skipping: data.hash_skipping, timebase_hz: hz,
        reference_scope: data.reference_scope ?? null,
        reference_explanation: 'Ordinary TextureCache lookups recompute the existing hash footprint instead of using scene memoization or unsafe interval skips. Surface-cache hits, DoubleBuffer block memoization, and existing footprint/import rules are outside this reference.',
        scope: 'Full-session self-tests and dedicated benchmark windows; independent of --start-ms/--end-ms.',
        tests_dropped: testsDropped,
        test_results_complete: testsDropped === null ? null : testsDropped === 0,
        test_counts: testCounts(tests), test_groups: groups, tests,
        failed_tests: tests.filter(test => test.status === 'fail')
            .map(({suite, category, name, detail}) => ({suite, category, name, detail})),
        benchmark: {
            order: benchmark.order, auto_start: benchmark.auto_start,
            warmup_seconds: benchmark.warmup_seconds, settle_seconds: benchmark.settle_seconds,
            measure_seconds: measureSeconds, requested: benchmark.requested,
            completed: benchmark.completed, aborted: benchmark.aborted,
            run_id: benchmark.run_id, frames_overflow: benchmark.frames_overflow,
            minimum_intervals_per_phase: MEMORY_MIN_INTERVALS,
            minimum_recorded_duration_ms: minDuration,
            comparison_exclusions: exclusions, phases, pairs,
            all_pairs_eligible: allPairsEligible,
            paired_summary: allPairsEligible ? {
                pairs: pairs.length,
                median_loops_per_second_delta_pct: quantile(loopDeltas, 0.5),
                min_loops_per_second_delta_pct: Math.min(...loopDeltas),
                max_loops_per_second_delta_pct: Math.max(...loopDeltas)
            } : null,
            interpretation: 'Paired render-loop observations, not display FPS or a correctness proof. Positive cadence delta means more loops/s; negative interval delta means shorter intervals. No actual staging dedup, incomplete runs, overflow, or insufficient samples produce n/a comparisons.'
        },
        limitations: 'A skipped capability is not a failed self-test. Capability failures are listed separately from JIT correctness failures. Passing these cases does not prove complete Guest/HLE/GPU write coverage or the absence of false-clean resources; this A/B does not compare GPU output images. Store timings include the microbenchmark harness; no timer/loop overhead is subtracted.'
    };
}

function memoryValidationHtml(report) {
    const heading = '<h2>Memory validation and staging publication A/B</h2>';
    if (!report.available || !report.schema_supported)
        return `${heading}<p>${escapeHtml(report.explanation)}</p>`;
    const format = (value, digits = 2) => typeof value === 'number' && Number.isFinite(value)
        ? value.toFixed(digits) : 'n/a';
    const counts = values => `${values.pass} pass / ${values.fail} fail / ${values.skip} skip`
        + (values.unknown ? ` / ${values.unknown} unknown` : '');
    const incompleteTestsHtml = report.test_results_complete === true ? ''
        : `<p><strong>Test-result capture is incomplete or unconfirmed: ${report.tests_dropped === null ? 'dropped-result count not reported' : report.tests_dropped + ' results dropped'}. Missing results must not be interpreted as passed tests; paired performance deltas are withheld.</strong></p>`;
    const failureHtml = report.failed_tests.length
        ? `<p><strong>Recorded failures:</strong></p><ul>${report.failed_tests.map(test => `<li><b>${escapeHtml(test.category)} — ${escapeHtml(test.suite + '/' + test.name)}</b>: ${escapeHtml(test.detail)}</li>`).join('')}</ul>
<p>${report.test_groups.self_tests.fail ? 'Correctness self-tests failed; paired performance deltas are withheld.' : 'These individual capability or microbenchmark failures remain visible; a cadence result does not certify them.'} Unsupported capability probes recorded as skip are not promoted to failures.</p>`
        : '<p>No failure status was recorded in the listed tests; untested cases remain outside their coverage.</p>';
    const groupTitles = {
        self_tests: 'Correctness self-tests', capabilities: 'Hardware capabilities',
        store_microbenchmarks: 'Store microbenchmarks', other: 'Other checks'
    };
    const testHtml = Object.entries(groupTitles).map(([category, title]) => {
        const tests = report.tests.filter(test => test.category === category);
        if (!tests.length) return '';
        const rows = tests.map(test => `<tr><td>${escapeHtml(test.suite + '/' + test.name)}</td><td>${escapeHtml(test.status)}</td><td class="detail">${escapeHtml(test.detail)}</td><td>${format(test.iterations, 0)}</td><td>${format(test.ns_per_store)}</td></tr>`).join('');
        return `<h3>${title}: ${counts(report.test_groups[category])}</h3>
<div class="wide"><table><thead><tr><th>Test</th><th>Status</th><th>Detail</th><th>Iterations</th><th>ns/store</th></tr></thead><tbody>${rows}</tbody></table></div>`;
    }).join('');
    const benchmark = report.benchmark;
    const phaseRows = benchmark.phases.map(phase => `<tr><td>${escapeHtml(phase.index)}</td><td>${escapeHtml(phase.variant)}</td><td>${phase.complete ? 'complete' : 'incomplete'}</td><td>${format(phase.measured_intervals, 0)}</td><td>${format(phase.duration_ms === null ? null : phase.duration_ms / 1000)}</td><td>${format(phase.loops_per_second)}</td><td>${format(phase.p50_ms)} / ${format(phase.p95_ms)} / ${format(phase.p99_ms)}</td><td>${format(phase.publish_calls, 0)} / ${format(phase.publish_bytes, 0)}</td><td>${format(phase.dedup_calls, 0)} / ${format(phase.dedup_bytes, 0)}</td><td>${format(phase.excluded_intervals, 0)} / ${format(phase.excluded_duration_ms)}</td><td class="detail">${escapeHtml(phase.comparison_exclusions.join(', ') || 'ready')}</td></tr>`).join('');
    const pairRows = benchmark.pairs.map(pair => `<tr><td>${pair.pair}</td><td>${escapeHtml(pair.baseline_phase ?? 'n/a')} / ${escapeHtml(pair.dedup_phase ?? 'n/a')}</td><td>${format(pair.loops_per_second_delta_pct)}</td><td>${format(pair.p50_interval_delta_pct)}</td><td>${format(pair.p95_interval_delta_pct)}</td><td>${format(pair.p99_interval_delta_pct)}</td><td class="detail">${escapeHtml(pair.comparison_exclusions.join(', ') || 'eligible observation')}</td></tr>`).join('');
    const paired = benchmark.paired_summary;
    return `${heading}
<p>Strict texture Hash: <b>${report.strict_hash ? 'on' : 'not confirmed'}</b>; Hash skipping: <b>${report.hash_skipping === false ? 'off' : 'not confirmed'}</b>. ${counts(report.test_counts)}.</p>
<p>Reference scope: <b>${escapeHtml(report.reference_scope ?? 'not reported')}</b>. ${escapeHtml(report.reference_explanation)}</p>
<p>${escapeHtml(report.scope)}</p>${incompleteTestsHtml}${failureHtml}${testHtml}
<h3>Automated staging A/B phases</h3>
<p>Run: ${escapeHtml(benchmark.run_id ?? 'n/a')}; order: ${escapeHtml(benchmark.order ?? 'n/a')}; completed: ${benchmark.completed === true ? 'yes' : 'no'}; aborted: ${benchmark.aborted === true ? 'yes' : benchmark.aborted === false ? 'no' : 'unknown'}; overflow: ${escapeHtml(benchmark.frames_overflow ?? 'unknown')}.</p>
<p>Each phase requires at least ${benchmark.minimum_intervals_per_phase} intervals and ${format(benchmark.minimum_recorded_duration_ms === null ? null : benchmark.minimum_recorded_duration_ms / 1000)} s of measured intervals. Comparison exclusions: ${escapeHtml(benchmark.comparison_exclusions.join(', ') || 'none')}.</p>
<div class="wide"><table><thead><tr><th>Phase</th><th>Variant</th><th>Status</th><th>Intervals</th><th>Seconds</th><th>Loops/s</th><th>P50 / P95 / P99 ms</th><th>Publish calls / bytes</th><th>Dedup calls / bytes</th><th>Excluded settle/boundary intervals / ms</th><th>Exclusions</th></tr></thead><tbody>${phaseRows}</tbody></table></div>
<h3>Adjacent paired observations: dedup relative to baseline</h3>
<div class="wide"><table><thead><tr><th>Pair</th><th>Baseline / dedup phase</th><th>Loops/s Δ%</th><th>P50 interval Δ%</th><th>P95 interval Δ%</th><th>P99 interval Δ%</th><th>Status</th></tr></thead><tbody>${pairRows}</tbody></table></div>
<p>Median paired loops/s delta: ${paired ? format(paired.median_loops_per_second_delta_pct) + '%' : 'n/a'}${paired ? `; pair range ${format(paired.min_loops_per_second_delta_pct)}% to ${format(paired.max_loops_per_second_delta_pct)}%.` : '.'}</p>
<small>${escapeHtml(benchmark.interpretation)} Repeated matching game routes, clocks and temperatures are required to separate an implementation effect from workload variation. ${escapeHtml(report.limitations)}</small>`;
}

async function main() {
    const args = process.argv.slice(2);
    if (!args.length || args.includes('--help')) {
        console.log(usage);
        return;
    }
    const dir = args.shift();
    let start = 0, end = Infinity, htmlPath, traceJsonPath;
    while (args.length) {
        const option = args.shift();
        const value = args.shift();
        if (option === '--start-ms' && isFiniteNumber(value)) start = Number(value);
        else if (option === '--end-ms' && isFiniteNumber(value)) end = Number(value);
        else if (option === '--html' && value) htmlPath = value;
        else if (option === '--trace-json' && value) traceJsonPath = value;
        else throw new Error(usage);
    }
    if (!(start >= 0 && end > start)) throw new Error('Invalid time range');

    const manifest = JSON.parse(await readFile(join(dir, 'manifest.json'), 'utf8'));
    const measurements = csv(await readFile(join(dir, 'hotspots.csv'), 'utf8'));
    const frames = csv(await readFile(join(dir, 'frames.csv'), 'utf8'))
        .filter(f => Number(f.elapsed_ms) >= start && Number(f.elapsed_ms) <= end);
    // A render frame is only adjacent to its predecessor when IDs are consecutive.
    // Old TraceLab sessions dropped event-ring entries; treating gaps as one
    // giant frame caused fictitious 200-300ms stalls.
    const intervals = [];
    let frameIdGaps = 0;
    for (let i = 1; i < frames.length; ++i) {
        if (Number(frames[i].render_frame_id) !== Number(frames[i - 1].render_frame_id) + 1) {
            ++frameIdGaps;
            continue;
        }
        const delta = Number(frames[i].elapsed_ms) - Number(frames[i - 1].elapsed_ms);
        if (delta > 0) intervals.push(delta);
    }
    const frameStats = {
        count: frames.length,
        recorded_total: manifest.frames_recorded,
        frame_id_gaps: frameIdGaps,
        frame_buffer_overflow: manifest.frames_overflow ?? null,
        measured_intervals: intervals.length,
        mean_interval_ms: intervals.length
            ? intervals.reduce((a, b) => a + b, 0) / intervals.length : null,
        p50_ms: quantile(intervals, 0.5),
        p95_ms: quantile(intervals, 0.95),
        p99_ms: quantile(intervals, 0.99),
        over_33ms: intervals.filter(v => v > 33.333).length,
        over_50ms: intervals.filter(v => v > 50).length
    };
    frameStats.estimated_render_loops_per_sec = frameStats.mean_interval_ms
        ? 1000 / frameStats.mean_interval_ms : null;

    const totals = new Map();
    for (const row of measurements) {
        const t = Number(row.elapsed_ms);
        if (!(t >= start && t <= end)) continue;
        const metric = row.metric;
        const item = totals.get(metric) ?? {
            metric, calls: 0, bytes: 0, samples: 0,
            sampled_wall_ms: 0, estimated_wall_ms: null,
            max_sampled_us: 0
        };
        item.calls += Number(row.calls);
        item.bytes += Number(row.bytes);
        item.samples += Number(row.sampled_calls);
        item.sampled_wall_ms += Number(row.sampled_wall_ms);
        // Do not sum per-window extrapolations: windows with zero sampled
        // calls must not silently contribute a false zero to the estimate.
        // Pool all samples within the selected interval before extrapolating.
        item.max_sampled_us = Math.max(item.max_sampled_us, Number(row.max_sampled_us));
        totals.set(metric, item);
    }
    const hotspots = [...totals.values()]
        .filter(row => row.calls > 0)
        .map(row => ({
            ...row,
            estimated_wall_ms: row.samples
                ? row.sampled_wall_ms * row.calls / row.samples : null
        }))
        .sort((a,b) => (b.estimated_wall_ms ?? -1) - (a.estimated_wall_ms ?? -1));

    const traceData = await readFile(join(dir, 'trace.bin'));
    if (traceData.length < 24 || traceData.toString('ascii', 0, 6) !== 'TLABv1')
        throw new Error('Invalid TraceLab trace.bin header');
    const traceVersion = traceData.readUInt32LE(8);
    const traceEntrySize = traceData.readUInt32LE(12);
    const traceHz = Number(traceData.readBigUInt64LE(16));
    if (traceVersion !== 1 || traceEntrySize !== 24 || !traceHz ||
        (traceData.length - 24) % traceEntrySize)
        throw new Error('Unsupported or truncated TraceLab trace.bin');

    const hleByNid = new Map();
    let earliestTick = Infinity;
    let longFrames = 0;
    for (let i = 24; i + traceEntrySize <= traceData.length; i += traceEntrySize) {
        const tick = Number(traceData.readBigUInt64LE(i));
        earliestTick = Math.min(earliestTick, tick);
        const kind = traceData.readUInt16LE(i + 16);
        if (kind === 6) ++longFrames;
        // Historical v1 trace.bin: some HLE NIDs were individually logged.
        if (kind !== 5) continue;
        const nid = traceData.readUInt32LE(i + 8);
        const microseconds = traceData.readUInt32LE(i + 12);
        const name = '0x' + nid.toString(16).padStart(8, '0');
        const item = hleByNid.get(name) ?? {
            nid:name, samples:0, sampled_wall_ms:0, max_sampled_us:0
        };
        ++item.samples;
        item.sampled_wall_ms += microseconds / 1000;
        item.max_sampled_us = Math.max(item.max_sampled_us, microseconds);
        hleByNid.set(name, item);
    }

    // Schema v2: the complete per-NID sampled aggregates are exported to
    // nid-samples.csv, avoiding millions of high-frequency trace events.
    let nidWindow = 'historical trace events (potentially dropped)';
    if (manifest.schema_version >= 2) {
        const nidRows = csv(await readFile(join(dir, 'nid-samples.csv'), 'utf8'));
        hleByNid.clear();
        for (const row of nidRows) {
            const key = row.nid;
            const item = hleByNid.get(key) ?? {
                nid:key, samples:0, sampled_wall_ms:0, max_sampled_us:0
            };
            item.samples += Number(row.samples);
            item.sampled_wall_ms += Number(row.sampled_wall_ms);
            item.max_sampled_us = Math.max(item.max_sampled_us, Number(row.max_sampled_us));
            hleByNid.set(key, item);
        }
        nidWindow = 'full session (not restricted to --start-ms/--end-ms)';
    }
    const hle_nids = [...hleByNid.values()]
        .sort((a,b) => b.sampled_wall_ms - a.sampled_wall_ms).slice(0, 25);
    let threads = [];
    if (manifest.schema_version >= 2)
        threads = csv(await readFile(join(dir, 'threads.csv'), 'utf8'));

    const getCalls = name => totals.get(name)?.calls ?? 0;
    const getBytes = name => totals.get(name)?.bytes ?? 0;
    const experiment = {
        // Experimental telemetry is based on recorded runtime config and
        // counters; none of these values is an automatically safe speedup.
        requested_skip_interval: manifest.unsafe_texture_hash_skip_requested_interval ?? 0,
        remaining_skip_interval_at_exit: manifest.unsafe_texture_hash_skip_final_interval ?? 0,
        audit_mismatch_detected: manifest.unsafe_texture_hash_audit_mismatch_detected ?? false,
        audit_incomplete: manifest.unsafe_texture_hash_audit_incomplete ?? false,
        skipped_full_hashes: getCalls('texture_hash_unsafe_skipped'),
        avoided_hash_range_bytes: getBytes('texture_hash_unsafe_skipped'),
        full_hash_audits_after_skips: getCalls('texture_hash_unsafe_audit'),
        changed_hashes_after_skips: getCalls('texture_hash_unsafe_audit_mismatch'),
        unauditable_skips: getCalls('texture_hash_unsafe_unverified'),
        changed_full_verifications: getCalls('texture_hash_changed'),
        unchanged_full_verifications: getCalls('texture_hash_unchanged'),
        same_scene_reuse: getCalls('texture_hash_scene_reused'),
        warning: 'UNSAFE experimental mode: skipped checks can miss transient guest/HLE/GPU writes. Audits cannot prove correctness.'
    };

    // All values refer to the selected time window, unlike the full-session
    // HLE NID table. In older TraceLab sessions, these probes did not exist;
    // return null instead of implying there were no rejected candidates.
    const probeReasons = [
        ['eligible', 'texture_hash_probe_eligible'],
        ['warmup', 'texture_hash_probe_warmup'],
        ['previously_changed', 'texture_hash_probe_changed'],
        ['no_scene_epoch', 'texture_hash_probe_no_epoch'],
        ['no_prior_verification', 'texture_hash_probe_no_baseline'],
        ['epoch_nonmonotonic', 'texture_hash_probe_nonmonotonic'],
        ['audit_due', 'texture_hash_probe_audit_due'],
        ['import_or_replacement', 'texture_hash_probe_import_export'],
        ['no_source_range', 'texture_hash_probe_no_range'],
        ['invalid_mapping', 'texture_hash_probe_invalid_mapping']
    ];
    const reasonValues = Object.fromEntries(probeReasons.map(([label, metric]) =>
        [label, {calls: getCalls(metric), bytes: getBytes(metric)}]));
    const deltaMetrics = [
        ['delta_1', 'texture_hash_epoch_delta_1'],
        ['delta_2_to_7', 'texture_hash_epoch_delta_2_7'],
        ['delta_8_to_15', 'texture_hash_epoch_delta_8_15'],
        ['delta_16_to_31', 'texture_hash_epoch_delta_16_31'],
        ['delta_32_plus', 'texture_hash_epoch_delta_32plus']
    ];
    const epochValues = Object.fromEntries(deltaMetrics.map(([label, metric]) =>
        [label, {calls: getCalls(metric), bytes: getBytes(metric)}]));
    const measuredLoops = getCalls('render_loop');
    const considered = Object.values(reasonValues).reduce((sum, v) => sum + v.calls, 0);
    const probe = manifest.texture_hash_probe_interval === undefined ? {
        available: false,
        description: 'Older capture: texture hash rejection reasons were not recorded.'
    } : {
        available: true,
        interval: manifest.texture_hash_probe_interval,
        considered_cached_texture_checks: considered,
        eligible_fraction: considered ? reasonValues.eligible.calls / considered : null,
        probe_new_entries: getCalls('texture_hash_probe_new_entry'),
        probe_new_entry_bytes: getBytes('texture_hash_probe_new_entry'),
        reasons: reasonValues,
        epoch_delta_histogram: epochValues,
        scene_boundaries: getCalls('texture_scene_boundary'),
        context_switches: getCalls('texture_scene_context_switch'),
        epoch_rewinds: getCalls('texture_scene_epoch_rewind'),
        cross_context_epoch_collisions: getCalls('texture_scene_epoch_collision'),
        render_loops_in_window: measuredLoops,
        explanation: 'Eligibility is a counterfactual probe using the current verification history, not proof that skipping Hash would be safe. Per-context local epoch semantics are unchanged.'
    };

    const aggressive = {
        requested_interval: manifest.unsafe_texture_hash_recheck_requested_interval ?? 0,
        remaining_interval_at_exit: manifest.unsafe_texture_hash_recheck_final_interval ?? 0,
        skipped_full_hashes: getCalls('texture_hash_aggressive_skipped'),
        avoided_hash_range_bytes: getBytes('texture_hash_aggressive_skipped'),
        audited_rechecks_after_skips: getCalls('texture_hash_aggressive_audits'),
        quarantined_changed_textures: getCalls('texture_hash_aggressive_quarantines'),
        unauditable_evictions_or_mappings: getCalls('texture_hash_aggressive_unverified'),
        per_texture_quarantine_seen: manifest.unsafe_texture_hash_recheck_mismatch_seen ?? false,
        incomplete_audits_seen: manifest.unsafe_texture_hash_recheck_unverified_seen ?? false,
        // This change deliberately continues the test with other cache entries
        // after a mismatch; it is never proof the skipped content was safe.
        disclaimer: 'Aggressive reuse-count hash skipping is NOT safe without complete guest CPU/HLE/GPU write tracking; audits detect some stale results only after display.'
    };

    const guestWrite = manifest.guest_write_probe_mode === undefined ? {
        available: false,
        explanation: 'Older TraceLab recording has no JIT store experiment.'
    } : (() => {
        const fast = getCalls('guest_jit_fast_writes');
        const tracked = getCalls('guest_jit_texture_page_writes');
        const slow = getCalls('guest_jit_slow_writes');
        const slowTracked = getCalls('guest_jit_slow_texture_page_writes');
        const total = fast + slow;
        const textureRelated = tracked + slowTracked;
        return {
            available: true,
            mode: manifest.guest_write_probe_mode,
            mode_name: ['off', 'count', 'texture', 'guard'][manifest.guest_write_probe_mode] ?? 'unknown',
            successful_fast_stores: fast,
            successful_callback_stores: slow,
            successful_all_counted_stores: total,
            fast_stores_to_ever_texture_pages: tracked,
            callback_stores_to_ever_texture_pages: slowTracked,
            ever_texture_pages_registered: getCalls('guest_texture_pages_registered'),
            ever_texture_pages_final: manifest.guest_write_probe_texture_pages,
            guard_tainted_pages_at_exit: manifest.guest_write_guard_tainted_pages ?? 0,
            guard_full_hash_checks_forced: getCalls('texture_hash_guard_blocked'),
            guard_full_hash_bytes_forced: getBytes('texture_hash_guard_blocked'),
            related_store_fraction: total ? textureRelated / total : null,
            writes_per_render_loop: measuredLoops ? total / measuredLoops : null,
            texture_related_writes_per_render_loop: measuredLoops
                ? textureRelated / measuredLoops : null,
            caveat: manifest.guest_write_probe_mode === 3
                ? 'UNSAFE JIT-only guard: there are NO global write counters in this mode. JIT stores to known texture pages set a sticky taint bit. Tainted textures use full Hash; untainted textures may still have changed due to HLE/GPU writes or aliases. Periodic audit mismatches remain possible.'
                : 'Only guest JIT CPU stores are counted; ordinary stores use native JIT inline increments, fallback and exclusive stores use the C++ slow-path count. Texture-page membership is monotonic during the title (ever seen); it is NOT a page-dirty/coherence tracker and ignores HLE, GPU, aliases, palette-only inputs and the end page of cross-page stores. Off mode is the exact original JIT path.'
        };
    })();

    const memoryValidation = await readMemoryValidation(dir, manifest);
    const report = {
        title: manifest.title,
        measurement: 'Render loop intervals, not physical display scans or game simulation FPS',
        requested_window_ms: {start, end: end === Infinity ? null : end},
        frames: frameStats,
        hotspots,
        hle_nids,
        texture_hash_experiment: experiment,
        texture_hash_probe: probe,
        texture_hash_aggressive: aggressive,
        guest_write_probe: guestWrite,
        memory_validation: memoryValidation,
        hle_nids_scope: nidWindow,
        threads,
        trace: {
            long_render_intervals_over_66ms: longFrames,
            events_dropped: manifest.events_dropped,
            nid_samples_overflow: manifest.nid_samples_overflow ?? null,
            window_rows_overflow: manifest.window_rows_overflow ?? null,
            trace_bytes: manifest.trace_bytes,
            gpu_timestamps: manifest.gpu_timestamps,
            caveat: 'GPU execution duration is not measured. Per-metric estimates are sampled CPU-side wall time and often overlap.'
        },
        metadata: manifest.metadata
    };
    console.log(JSON.stringify(report, null, 2));

    if (htmlPath) {
        const guestWriteHtml = guestWrite.available
            ? `<h2>Guest JIT write-barrier cost experiment (complete texture Hash)</h2>
<p>Probe mode: <b>${escapeHtml(guestWrite.mode_name)}</b>;
fast stores: ${guestWrite.successful_fast_stores};
callback/exclusive stores: ${guestWrite.successful_callback_stores};
stores to ever-texture pages: ${guestWrite.fast_stores_to_ever_texture_pages + guestWrite.callback_stores_to_ever_texture_pages};
unique texture pages: ${guestWrite.ever_texture_pages_final};
writes per render loop: ${guestWrite.writes_per_render_loop === null ? 'n/a' : guestWrite.writes_per_render_loop.toFixed(1)};
sticky JIT-CPU-tainted pages: ${guestWrite.guard_tainted_pages_at_exit};
Hash skip attempts blocked by taint: ${guestWrite.guard_full_hash_checks_forced};
bytes forced back to full Hash: ${guestWrite.guard_full_hash_bytes_forced}.</p>
<small>${escapeHtml(guestWrite.caveat)}</small>`
            : '';
        const probeRows = probe.available ? Object.entries(probe.reasons)
            .sort((a,b) => b[1].calls - a[1].calls)
            .map(([name,v]) => `<tr><td>${escapeHtml(name)}</td><td>${v.calls}</td><td>${v.bytes}</td></tr>`)
            .join('') : '';
        const probeHtml = probe.available
            ? `<h2>Texture hash probe (no changes to cache validity)</h2>
<p>Probe interval: ${probe.interval}; cached checks classified: ${probe.considered_cached_texture_checks};
eligible share: ${probe.eligible_fraction === null ? 'n/a' : (probe.eligible_fraction * 100).toFixed(2) + '%'}.
Scene boundaries: ${probe.scene_boundaries}; context switches: ${probe.context_switches};
epoch rewinds: ${probe.epoch_rewinds}; cross-context collisions: ${probe.cross_context_epoch_collisions}.</p>
<table><thead><tr><th>Reason</th><th>Calls</th><th>Texture source range bytes</th></tr></thead><tbody>${probeRows}</tbody></table>`
            : '<p>Texture hash probe not present in this older capture.</p>';
        const rows = hotspots.map(item => `<tr><td>${escapeHtml(item.metric)}</td><td>${item.calls}</td><td>${item.bytes}</td><td>${item.samples}</td><td>${item.estimated_wall_ms === null ? 'n/a' : item.estimated_wall_ms.toFixed(3)}</td></tr>`).join('');
        const html = `<!doctype html><meta charset="utf-8"><title>Vita3K TraceLab</title>
<style>body{font:15px system-ui;margin:3rem auto;max-width:980px;padding:0 1rem;color:#222}
table{border-collapse:collapse;width:100%}td,th{border-bottom:1px solid #ddd;padding:.6rem;text-align:right}
td:first-child,th:first-child{text-align:left}small{color:#666}.wide{overflow-x:auto}.detail{text-align:left;min-width:14rem}</style>
<h1>Vita3K TraceLab – ${escapeHtml(manifest.title)}</h1>
<p>Render-loop cadence: ${frameStats.estimated_render_loops_per_sec?.toFixed(2) ?? 'n/a'}/s;
P50/P95/P99 intervals: ${frameStats.p50_ms?.toFixed(2) ?? 'n/a'} /
${frameStats.p95_ms?.toFixed(2) ?? 'n/a'} /
${frameStats.p99_ms?.toFixed(2) ?? 'n/a'} ms.</p>
<p>Trace events dropped: ${manifest.events_dropped}; frame-ID gaps: ${frameIdGaps}; GPU timestamps: ${escapeHtml(manifest.gpu_timestamps)}.</p>
${memoryValidationHtml(memoryValidation)}
${guestWriteHtml}
<h2>Aggressive reuse-count Hash experiment (UNSAFE)</h2>
<p>Requested every-Nth-check interval: ${aggressive.requested_interval || 'off'};
skipped full hashes: ${aggressive.skipped_full_hashes};
avoided source hash range bytes: ${aggressive.avoided_hash_range_bytes};
full-hash audits: ${aggressive.audited_rechecks_after_skips};
per-texture quarantine events: ${aggressive.quarantined_changed_textures};
unverifiable evictions/mappings: ${aggressive.unauditable_evictions_or_mappings}.
${aggressive.per_texture_quarantine_seen ? '<strong>CONTENT CHANGE AFTER SKIP: possible stale frames.</strong>' : ''}
${aggressive.incomplete_audits_seen ? '<strong>Some skipped ranges could not be audited.</strong>' : ''}</p>
<small>Not a production-safe optimization. A zero-error audit still cannot prove that Guest/HLE/GPU writes were seen.</small>
<h2>Historical scene-epoch texture hash experiment (UNSAFE)</h2>
<p>Requested audit interval: ${experiment.requested_skip_interval || 'off'};
skipped hashes: ${experiment.skipped_full_hashes};
avoided hash range bytes: ${experiment.avoided_hash_range_bytes};
full-hash audits after skips: ${experiment.full_hash_audits_after_skips};
audit mismatches: ${experiment.changed_hashes_after_skips};
unverifiable invalidations: ${experiment.unauditable_skips}.
${experiment.audit_mismatch_detected ? '<strong>EXPERIMENT STOPPED DUE TO MISMATCH.</strong>' : ''}
${experiment.audit_incomplete ? '<strong>EXPERIMENT STOPPED: AUDIT INCOMPLETE.</strong>' : ''}</p>
<small>CPU-side wall-time estimates may overlap and include preemption/driver waits.
Do not add phases or treat rendered-loop intervals as game simulation FPS.</small>
${probeHtml}
<h2>Hotspots</h2><table><thead><tr><th>Metric</th><th>Calls</th><th>Bytes</th><th>Samples</th><th>Estimated wall ms</th></tr></thead><tbody>${rows}</tbody></table>`;
        await writeFile(htmlPath, html);
    }

    if (traceJsonPath) {
        const buf = traceData;
        const entrySize = traceEntrySize;
        const tickHz = traceHz;
        const names = {
            1:'render_begin', 2:'render_end', 3:'gxm_new_frame', 4:'batch_consume',
            5:'hle_nid_sample', 6:'long_frame', 7:'pipeline_compiled', 8:'jit_cache_recycle',
            9:'unsafe_texture_hash_audit_mismatch'
        };
        const events = [];
        const firstTick = Number.isFinite(earliestTick) ? earliestTick : 0;
        // Multiple thread rings are drained in slot order, not timestamp order.
        // Trace viewers can sort by ts; keep a common clock origin.
        for (let offset = 24; offset + entrySize <= buf.length; offset += entrySize) {
            const tick = Number(buf.readBigUInt64LE(offset));
            const correlation = buf.readUInt32LE(offset + 8);
            const value = buf.readUInt32LE(offset + 12);
            const kind = buf.readUInt16LE(offset + 16);
            const threadSlot = buf.readUInt16LE(offset + 18);
            events.push({
                name: names[kind] ?? `event_${kind}`, cat:'tracelab',
                ph:'i', s:'t', ts:(tick - firstTick) * 1e6 / tickHz,
                pid:1, tid:threadSlot, args:{correlation,value}
            });
        }
        await writeFile(traceJsonPath, JSON.stringify({traceEvents:events}));
    }
}
main().catch(error => {console.error(error.message);process.exitCode=1;});
