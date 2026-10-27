#!/usr/bin/env node
// Analyze one matching six-CSV Switch profiling session without dependencies.
// This reads the inputs only; stdout is a JSON report, not a performance claim.
import { readFile, readdir } from 'node:fs/promises';
import { createHash } from 'node:crypto';
import { join } from 'node:path';

const prefixes = ['cpu-threads', 'render-phases', 'gxm-opcodes',
    'gxm-state-draw', 'lsfg-state', 'frame-events'];
const numericColumns = {
    'cpu-threads': ['delta_total_ticks', 'tick_frequency', 'priority', 'self_total_pct',
        'core0_ms', 'core1_ms', 'core2_ms', 'core3_ms'],
    'render-phases': ['count', 'total_wall_ms', 'max_wall_ms'],
    'gxm-opcodes': ['calls', 'sampled_calls', 'estimated_total_wall_ms'],
    'gxm-state-draw': ['calls', 'sampled_calls', 'estimated_total_wall_ms'],
    'lsfg-state': ['requested', 'compatible', 'high_fps_passthrough', 'source_present_calls',
        'generated_frames', 'actually_generated'],
    'frame-events': ['sequence', 'value']
};
const categoryColumns = {
    'cpu-threads': ['label', 'affinity_mask', 'core_query_rc', 'total_query_rc'],
    'render-phases': ['phase'], 'gxm-opcodes': ['opcode'],
    'gxm-state-draw': ['metric'], 'lsfg-state': [], 'frame-events': ['event']
};
const usage = 'Usage: node scripts/analyze-switch-performance.mjs <log-directory> [--start-ms N] [--end-ms N]';

function parseCsv(text, name) {
    const records = [];
    let row = [], field = '', quoted = false;
    for (let i = 0; i < text.length; ++i) {
        const c = text[i];
        if (c === '"') {
            if (quoted && text[i + 1] === '"') { field += '"'; ++i; }
            else quoted = !quoted;
        } else if (!quoted && c === ',') {
            row.push(field); field = '';
        } else if (!quoted && c === '\n') {
            row.push(field.replace(/\r$/, ''));
            if (row.some(value => value !== '')) records.push(row);
            row = []; field = '';
        } else field += c;
    }
    if (quoted) throw new Error(`${name}: unterminated quoted field`);
    if (row.length || field) { row.push(field.replace(/\r$/, '')); records.push(row); }
    const header = records.shift();
    if (!header?.includes('elapsed_ms') || !records.length)
        throw new Error(`${name}: missing header or data`);
    return records.map((record, index) => {
        if (record.length !== header.length)
            throw new Error(`${name}:${index + 2}: inconsistent column count`);
        const result = Object.fromEntries(header.map((key, i) => {
            const value = record[i];
            return [key, /^-?\d+(?:\.\d+)?$/.test(value) ? Number(value) : value];
        }));
        if (!Number.isFinite(result.elapsed_ms))
            throw new Error(`${name}:${index + 2}: invalid elapsed_ms`);
        return result;
    });
}

const sum = values => values.reduce((a, b) => a + b, 0);
const total = (rows, field) => sum(rows.map(row => row[field]));
const group = (rows, field) => {
    const result = new Map();
    for (const row of rows) {
        if (!result.has(row[field])) result.set(row[field], []);
        result.get(row[field]).push(row);
    }
    return result;
};
function stats(values) {
    if (!values.length) return { count: 0 };
    const sorted = values.slice().sort((a, b) => a - b);
    const percentile = p => {
        const index = (sorted.length - 1) * p;
        const lo = Math.floor(index), hi = Math.ceil(index);
        return sorted[lo] + (sorted[hi] - sorted[lo]) * (index - lo);
    };
    return { count: values.length, total: sum(values), mean: sum(values) / values.length,
        min: sorted[0], p50: percentile(0.5), p95: percentile(0.95),
        p99: percentile(0.99), max: sorted.at(-1) };
}

async function main() {
    const args = process.argv.slice(2);
    if (args.includes('--help')) { console.log(usage); return; }
    const directory = args.shift();
    if (!directory) throw new Error(usage);
    let requestedStart = 0, requestedEnd = Infinity;
    while (args.length) {
        const option = args.shift(), value = Number(args.shift());
        if (!['--start-ms', '--end-ms'].includes(option) || !Number.isFinite(value) || value < 0)
            throw new Error(usage);
        if (option === '--start-ms') requestedStart = value;
        else requestedEnd = value;
    }
    if (requestedStart >= requestedEnd) throw new Error('start-ms must be less than end-ms');

    const names = await readdir(directory), data = {}, files = {};
    let suffix;
    for (const prefix of prefixes) {
        const matching = names.filter(name => name.startsWith(`${prefix}-`) && name.endsWith('.csv'));
        if (matching.length !== 1)
            throw new Error(`Expected exactly one ${prefix}-*.csv; use a directory containing one session`);
        const name = matching[0], currentSuffix = name.slice(prefix.length + 1);
        if (suffix !== undefined && currentSuffix !== suffix)
            throw new Error(`Mismatched session suffix: ${name}`);
        suffix = currentSuffix;
        const bytes = await readFile(join(directory, name));
        data[prefix] = parseCsv(bytes.toString('utf8').replace(/^\uFEFF/, ''), name);
        for (const row of data[prefix]) {
            for (const column of numericColumns[prefix])
                if (!Number.isFinite(row[column])) throw new Error(`${name}: missing or invalid ${column}`);
            for (const column of categoryColumns[prefix])
                if (row[column] === undefined || row[column] === '') throw new Error(`${name}: missing ${column}`);
            if (prefix === 'cpu-threads' && (Number(row.total_query_rc) !== 0 || row.tick_frequency <= 0))
                throw new Error(`${name}: failed total CPU query or invalid tick frequency; cannot aggregate CPU`);
        }
        files[prefix] = { name, rows: data[prefix].length,
            sha256: createHash('sha256').update(bytes).digest('hex') };
    }

    const polls = data['lsfg-state'].map(row => row.elapsed_ms);
    if (polls.some((time, i) => i > 0 && time <= polls[i - 1]))
        throw new Error('LSFG sample boundaries must be strictly increasing');
    for (const prefix of prefixes.filter(name => name !== 'frame-events')) {
        const times = [...new Set(data[prefix].map(row => row.elapsed_ms))];
        if (JSON.stringify(times) !== JSON.stringify(polls))
            throw new Error(`${prefix}: sample boundaries do not match the LSFG timeline`);
    }
    // Start rounds up and end rounds down to common sample boundaries.
    // Rows are window endpoints, so include start < elapsed_ms <= end.
    const start = [0, ...polls].find(time => time >= requestedStart);
    const end = polls.filter(time => time <= requestedEnd).at(-1);
    if (start === undefined || end === undefined || end <= start)
        throw new Error('No complete sampling interval inside the requested bounds');
    const duration = end - start;
    const selected = name => data[name].filter(row => row.elapsed_ms > start && row.elapsed_ms <= end);
    const renderLoops = total(selected('render-phases').filter(row => row.phase === 'process_batches'), 'count');
    if (!renderLoops) throw new Error('Selected interval contains no completed process_batches samples');

    const phases = [...group(selected('render-phases'), 'phase')].map(([phase, rows]) => ({
        phase, count: total(rows, 'count'), wall_ms: total(rows, 'total_wall_ms'),
        ms_per_render_loop: total(rows, 'total_wall_ms') / renderLoops,
        max_ms: Math.max(...rows.map(row => row.max_wall_ms))
    })).filter(row => row.count).sort((a, b) => b.wall_ms - a.wall_ms);
    const sampled = (name, field) => [...group(selected(name), field)].map(([metric, rows]) => ({
        metric, calls: total(rows, 'calls'), sampled_calls: total(rows, 'sampled_calls'),
        calls_per_render_loop: total(rows, 'calls') / renderLoops,
        estimated_wall_ms: total(rows, 'estimated_total_wall_ms'),
        estimated_ms_per_render_loop: total(rows, 'estimated_total_wall_ms') / renderLoops
    })).sort((a, b) => b.estimated_wall_ms - a.estimated_wall_ms);
    const events = Object.fromEntries([...group(selected('frame-events'), 'event')].map(([event, rows]) => {
        const intervals = rows.slice(1).map((row, i) => row.elapsed_ms - rows[i].elapsed_ms);
        return [event, { count: rows.length, count_per_second: rows.length * 1000 / duration,
            interval_rate_per_second: intervals.length ? intervals.length * 1000 / sum(intervals) : null,
            interval_ms: stats(intervals), over_50_ms: intervals.filter(value => value > 50).length,
            over_100_ms: intervals.filter(value => value > 100).length }];
    }));
    // These exact counters extend the existing CSV without changing its schema.
    // Old six-file recordings have only the two draw-skip counters; an absent
    // diagnostic must not be reported as a measured 0% cache hit rate.
    const fineCounters = [...group(selected('gxm-state-draw')
        .filter(row => row.measurement === 'counter_only'), 'metric')]
        .map(([metric, rows]) => ({ metric, calls: total(rows, 'calls'),
            calls_per_render_loop: total(rows, 'calls') / renderLoops }))
        .sort((a, b) => a.metric.localeCompare(b.metric));
    const counter = name => fineCounters.find(row => row.metric === name)?.calls ?? 0;
    const descriptorModes = ['desc_bind_skipped', 'desc_bind_partial',
        'desc_bind_full', 'desc_bind_unconditional'];
    const descriptorBindCalls = sum(descriptorModes.map(counter));
    const cpuMs = row => row.delta_total_ticks * 1000 / row.tick_frequency;
    const cpu = [...group(selected('cpu-threads'), 'label')].map(([label, rows]) => ({
        label, cpu_ms: sum(rows.map(cpuMs)), one_core_equivalent_pct: sum(rows.map(cpuMs)) * 100 / duration,
        priorities: [...new Set(rows.map(row => row.priority))],
        affinity_masks: [...new Set(rows.map(row => row.affinity_mask))],
        self_pct: stats(rows.filter(row => row.self_total_pct >= 0).map(row => row.self_total_pct))
    })).sort((a, b) => b.cpu_ms - a.cpu_ms);

    const allEvents = data['frame-events'], loops = [];
    let loop;
    for (const event of allEvents) {
        if (event.event === 'render_loop_begin') {
            if (loop) loops.push({ ...loop, next_begin_ms: event.elapsed_ms });
            loop = { begin_ms: event.elapsed_ms, new_frames: 0 };
        }
        if (loop && event.event === 'render_swap_done') loop.swap_done_ms = event.elapsed_ms;
        if (loop && event.event === 'gxm_new_frame') ++loop.new_frames;
    }
    const completeLoops = loops.filter(row => row.begin_ms > start && row.begin_ms <= end && row.swap_done_ms !== undefined);
    const gaps = completeLoops.map(row => row.next_begin_ms - row.swap_done_ms);
    const largeGaps = completeLoops.filter(row => row.next_begin_ms - row.swap_done_ms > 3).map(row => {
        const previousPoll = polls.filter(time => time <= row.swap_done_ms).at(-1);
        return { swap_done_ms: row.swap_done_ms, gap_ms: row.next_begin_ms - row.swap_done_ms,
            preceding_poll_ms: previousPoll ?? null };
    });
    const allCpu = data['cpu-threads'];
    const report = {
        files,
        interval: { requested_start_ms: requestedStart, requested_end_ms: Number.isFinite(requestedEnd) ? requestedEnd : null,
            start_exclusive_ms: start, end_inclusive_ms: end, duration_ms: duration,
            windows: selected('lsfg-state').length, render_loops: renderLoops },
        data_quality: {
            sequence_gaps: (allEvents[0].sequence === 1 ? 0 : 1)
                + allEvents.slice(1).filter((row, i) => row.sequence !== allEvents[i].sequence + 1).length,
            timestamp_backtracks: allEvents.slice(1).filter((row, i) => row.elapsed_ms < allEvents[i].elapsed_ms).length,
            dropped_events: total(allEvents.filter(row => row.event === 'events_dropped'), 'value'),
            source_event_count: allEvents.filter(row => row.event === 'source_present').length,
            lsfg_source_count: total(data['lsfg-state'], 'source_present_calls'),
            all_core_query_rc: [...new Set(allCpu.map(row => row.core_query_rc))],
            all_total_query_rc: [...new Set(allCpu.map(row => row.total_query_rc))],
            all_per_core_cpu_ms: sum(allCpu.map(row => row.core0_ms + row.core1_ms + row.core2_ms + row.core3_ms)),
            all_kernel_cpu_ms: sum(allCpu.map(cpuMs))
        },
        lsfg_full_session: Object.fromEntries(['requested', 'compatible', 'high_fps_passthrough',
            'generated_frames', 'actually_generated'].map(field => [field, stats(data['lsfg-state'].map(row => row[field]))])),
        skipped_draws_full_session: Object.fromEntries(['draw_skipped_mask', 'draw_skipped_pipeline'].map(metric =>
            [metric, total(data['gxm-state-draw'].filter(row => row.metric === metric), 'calls')])),
        events, phases, opcodes: sampled('gxm-opcodes', 'opcode'),
        fine_metrics: sampled('gxm-state-draw', 'metric'), fine_counters: fineCounters,
        hotpath_diagnostics: {
            available: descriptorBindCalls > 0,
            descriptor_bind: {
                total: descriptorBindCalls,
                skipped: counter('desc_bind_skipped'),
                partial: counter('desc_bind_partial'),
                full: counter('desc_bind_full'),
                unconditional: counter('desc_bind_unconditional'),
                skipped_pct: descriptorBindCalls > 0 ?
                    100 * counter('desc_bind_skipped') / descriptorBindCalls : null
            },
            descriptor_updates: {
                vertex: counter('desc_vert_update'),
                fragment: counter('desc_frag_update'),
                pool_grows: counter('desc_pool_grow')
            },
            texture_resolution: {
                color_surface_hit: counter('texture_color_hit'),
                depth_surface_hit: counter('texture_depth_hit'),
                texture_cache_fallback: counter('texture_cache_fallback'),
                descriptor_changed: counter('texture_descriptor_changed'),
                descriptor_reused: counter('texture_descriptor_reused')
            },
            index_ranges: {
                rescanned: counter('index_range_scanned'),
                reused: counter('index_range_reused')
            }
        },
        cpu,
        complete_loop_new_frames: stats(completeLoops.map(row => row.new_frames)),
        post_swap_gap_ms: stats(gaps), post_swap_gaps_over_3_ms: largeGaps,
        interpretation: [
            'Events measure software cadence, not physical scanout or unique game simulation frames.',
            'Percentiles use linear interpolation at (N - 1) * p; only within-interval adjacent events are used.',
            'Phases and sampled child metrics are nested wall times, not additive CPU or GPU costs.',
            'Sampled estimates sum the CSV window estimates; fixed opcode sampling may alias repeating workloads.',
            'CPU percentages use kernel total ticks divided by the selected common interval, not summed per-core fields.',
            'Self CPU samples have independent checkpoint windows. Post-swap gaps alone do not prove a mutex wait.',
            'Hotpath counters are exact for instrumented calls only; an absent metric is not evidence of a zero hit rate.',
            'CPU fine-timer sub-stages are sampled wall time and cannot establish GPU saturation.',
            'Do not claim exact cadence percentiles if the quality report shows dropped or missing events.'
        ]
    };
    console.log(JSON.stringify(report, null, 2));
}

main().catch(error => { console.error(error.message); process.exitCode = 1; });
