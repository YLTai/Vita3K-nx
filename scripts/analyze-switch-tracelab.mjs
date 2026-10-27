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
    const intervals = frames.map(f => Number(f.interval_ms))
        .filter((value, i) => value > 0 && i > 0);
    const frameStats = {
        count: frames.length,
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
            sampled_wall_ms: 0, estimated_wall_ms: 0,
            max_sampled_us: 0
        };
        item.calls += Number(row.calls);
        item.bytes += Number(row.bytes);
        item.samples += Number(row.sampled_calls);
        item.sampled_wall_ms += Number(row.sampled_wall_ms);
        // This extrapolation describes the same metric only; nested
        // metrics overlap and must NEVER be summed into a frame budget.
        item.estimated_wall_ms += Number(row.estimated_wall_ms);
        item.max_sampled_us = Math.max(item.max_sampled_us, Number(row.max_sampled_us));
        totals.set(metric, item);
    }
    const hotspots = [...totals.values()]
        .filter(row => row.calls > 0)
        .sort((a, b) => b.estimated_wall_ms - a.estimated_wall_ms);
    // The binary event stream carries sampled HLE NIDs (not full guest call
    // stacks). Count them without assuming one sample equals one call.
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
        if (kind !== 5) continue;
        const nid = traceData.readUInt32LE(i + 8);
        const microseconds = traceData.readUInt32LE(i + 12);
        const name = '0x' + nid.toString(16).padStart(8, '0');
        const item = hleByNid.get(name) ?? {nid:name,samples:0,sampled_wall_ms:0,max_sampled_us:0};
        ++item.samples;
        item.sampled_wall_ms += microseconds / 1000;
        item.max_sampled_us = Math.max(item.max_sampled_us, microseconds);
        hleByNid.set(name, item);
    }
    const hle_nids = [...hleByNid.values()]
        .sort((a,b) => b.sampled_wall_ms - a.sampled_wall_ms).slice(0, 25);
    const report = {
        title: manifest.title,
        measurement: 'Render loop intervals, not physical display scans or game simulation FPS',
        requested_window_ms: {start, end: end === Infinity ? null : end},
        frames: frameStats,
        hotspots,
        hle_nids,
        trace: {
            long_render_intervals_over_66ms: longFrames,
            events_dropped: manifest.events_dropped,
            trace_bytes: manifest.trace_bytes,
            gpu_timestamps: manifest.gpu_timestamps,
            caveat: 'GPU execution duration is not measured. Per-metric estimates are sampled CPU-side wall time and often overlap.'
        },
        metadata: manifest.metadata
    };
    console.log(JSON.stringify(report, null, 2));

    if (htmlPath) {
        const rows = hotspots.map(item => `<tr><td>${escapeHtml(item.metric)}</td><td>${item.calls}</td><td>${item.bytes}</td><td>${item.samples}</td><td>${item.estimated_wall_ms.toFixed(3)}</td></tr>`).join('');
        const html = `<!doctype html><meta charset="utf-8"><title>Vita3K TraceLab</title>
<style>body{font:15px system-ui;margin:3rem auto;max-width:980px;padding:0 1rem;color:#222}
table{border-collapse:collapse;width:100%}td,th{border-bottom:1px solid #ddd;padding:.6rem;text-align:right}
td:first-child,th:first-child{text-align:left}small{color:#666}</style>
<h1>Vita3K TraceLab – ${escapeHtml(manifest.title)}</h1>
<p>Render-loop cadence: ${frameStats.estimated_render_loops_per_sec?.toFixed(2) ?? 'n/a'}/s;
P50/P95/P99 intervals: ${frameStats.p50_ms?.toFixed(2) ?? 'n/a'} /
${frameStats.p95_ms?.toFixed(2) ?? 'n/a'} /
${frameStats.p99_ms?.toFixed(2) ?? 'n/a'} ms.</p>
<p>Trace events dropped: ${manifest.events_dropped}; GPU timestamps: ${escapeHtml(manifest.gpu_timestamps)}.</p>
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
            5:'hle_nid_sample', 6:'long_frame', 7:'pipeline_compiled', 8:'jit_cache_recycle'
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
