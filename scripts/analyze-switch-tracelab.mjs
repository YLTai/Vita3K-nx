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

    const report = {
        title: manifest.title,
        measurement: 'Render loop intervals, not physical display scans or game simulation FPS',
        requested_window_ms: {start, end: end === Infinity ? null : end},
        frames: frameStats,
        hotspots,
        hle_nids,
        texture_hash_experiment: experiment,
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
        const rows = hotspots.map(item => `<tr><td>${escapeHtml(item.metric)}</td><td>${item.calls}</td><td>${item.bytes}</td><td>${item.samples}</td><td>${item.estimated_wall_ms === null ? 'n/a' : item.estimated_wall_ms.toFixed(3)}</td></tr>`).join('');
        const html = `<!doctype html><meta charset="utf-8"><title>Vita3K TraceLab</title>
<style>body{font:15px system-ui;margin:3rem auto;max-width:980px;padding:0 1rem;color:#222}
table{border-collapse:collapse;width:100%}td,th{border-bottom:1px solid #ddd;padding:.6rem;text-align:right}
td:first-child,th:first-child{text-align:left}small{color:#666}</style>
<h1>Vita3K TraceLab – ${escapeHtml(manifest.title)}</h1>
<p>Render-loop cadence: ${frameStats.estimated_render_loops_per_sec?.toFixed(2) ?? 'n/a'}/s;
P50/P95/P99 intervals: ${frameStats.p50_ms?.toFixed(2) ?? 'n/a'} /
${frameStats.p95_ms?.toFixed(2) ?? 'n/a'} /
${frameStats.p99_ms?.toFixed(2) ?? 'n/a'} ms.</p>
<p>Trace events dropped: ${manifest.events_dropped}; frame-ID gaps: ${frameIdGaps}; GPU timestamps: ${escapeHtml(manifest.gpu_timestamps)}.</p>
<h2>Texture hash experiment (UNSAFE)</h2>
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
