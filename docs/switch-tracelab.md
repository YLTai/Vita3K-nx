# Vita3K-nx TraceLab v2 — RAM-first diagnostic capture

This edition is intended for **slow TF/microSD cards**. It separates high
frequency measurements from storage completely: during an active game session,
the new TraceLab collects compact atomic counters, per-thread bounded event
queues and bounded RAM snapshots. It performs **no SD-card writes while the
game is running**. All output is written in batches when the game exits
normally, after guest/render workers have stopped.

The previous six-CSV profiler was a second source of SD-card stalls.
**It is now disabled by default in TraceLab builds**, including when
`mode=off`. It can be explicitly re-enabled if an old diagnostic comparison
requires its thread and stage CSVs.

## Config file

Create or edit `sdmc:/switch/vita3k/tracelab.cfg`, for example:

```ini
# Read at the beginning of every game session; no rebuild needed.
mode=auto
legacy_csv=off
```

| Setting | Purpose |
| --- | --- |
| `mode=off` | No TraceLab counters, timers or exports; suitable for a matched-build baseline |
| `mode=light` | Counter-only stage totals, render frames and sparse event trace; no sampled stage timers |
| `mode=cpu` | Counters plus sampled CPU/JIT/HLE phase wall time |
| `mode=render` | Counters plus sampled GXM/renderer/Vulkan stage wall time |
| `mode=auto` | **Default**: both groups; random ~1/32 stopwatch samples |
| `mode=deep` | Both groups; random ~1/8 stopwatch samples plus render-begin and per-batch events |
| `legacy_csv=on` | Also run old profiler: six older files, frequent synchronous writes and more overhead |

The configuration is independent of the launcher's regenerated `config.yml`.
Start with `auto` and `legacy_csv=off`. Do **not** use `deep` for FPS
comparison; it has more overhead. For recording old per-core CPU tick and
fine-grain render phase CSVs, run a separate session with `legacy_csv=on`.
Never compare different profiler modes as if they were identical hardware loads.

## What is recorded

TraceLab measures these categories: Dynarmic JIT Run and code-cache recycling;
sampled HLE calls grouped by NID; Guest scheduling acquisition/yield; GXM
front-end Draw and uniform/state preparation; command pool and batch handling;
texture lookups, hashing, hash changed/unchanged/scene-reused outcomes, uploads;
index scans and vertex binds; External Host cache clean/flush byte ranges;
descriptor set resolve/update/bind; pipeline lookup and worker compilation;
Vulkan queue submit/present; render loop timing; and the collector's own CPU
side polling cost.

New counters for texture hash outcomes are **observational only**: the cache
invalidation, hashing, GPU sync and upload behavior remain unchanged.

`threads.csv` additionally captures available Horizon **total CPU tick**
deltas from in-thread checkpoints, rather than relying on the known-unreliable
per-core percentages. A label identifies guest, render, frontend, or shader
worker when available. CPU times may miss up to the final checkpoint period
and cannot independently separate HLE waiting from actual computation.

### A complete session directory

After *normal exit*, look under
`sdmc:/switch/vita3k/logs/tracelab-<TITLE>-<epoch>-<run>/`:

```text
manifest.json      schema=2, modes, timebase, frame counts, overflow flags
hotspots.csv       ~1 Hz counter and random-sampled stopwatch snapshots
frames.csv         independently recorded render-loop completion timestamps
nid-samples.csv    sampled HLE NIDs aggregated in RAM by producer thread
threads.csv        self CPU total tick checkpoints for each producer
trace.bin          sparse binary events, sorted by CPU tick on normal exit
summary.json       full-session metric totals and sample tick sums
```

**No files are written by TraceLab while the game is running.** File creation,
CSV formatting and `fwrite` happen during normal exit and may be slow on a
slow TF card. This trade-off preserves gameplay measurement fidelity.
**A crash or forced HOME exit may lose the entire in-RAM TraceLab session.**
Legacy CSVs, if enabled, have their old write/flush behavior.

### Bounded RAM usage and data integrity

TraceLab bounds its allocation sizes (per-session, not continually growing):

- Up to 64 registered producer slots.
- Up to 1024 pending events per slot; up to 65,536 preserved trace events
  (~1.5 MiB of binary trace payload).
- Up to 32,768 independently recorded render-loop completions
  (~512 KiB in RAM, about 22 minutes at 25/s).
- Up to 32,768 metric-window rows (compact in-memory records).
- Up to 128 distinct sampled NIDs per producer; rare overflow is explicitly counted.

`manifest.json` records `frames_overflow`, `events_dropped`,
`window_rows_overflow` and `nid_samples_overflow`. If nonzero, report the
lost coverage rather than inventing a complete percentile or hotspot ranking.

The RenderEnd timestamp now goes into its **own fixed frame array**, not
through the event queue. Missing non-frame trace events therefore do not
silently produce huge fake frame intervals. Only frames beyond the declared
frame array limit are omitted from `frames.csv`.

### Interpreting measurements

- `hotspots.csv` retains the v1 schema for compatibility; `thread_slot=0`
  means the new RAM aggregator has **merged all producers** for that metric
  in that time window. Exact per-thread CPU checkpoints are in `threads.csv`.
- `estimated_wall_ms` of a 1-second row is meaningful only if
  `sampled_calls > 0`. The PC analyzer now **pools samples across all selected
  windows before extrapolating**, instead of summing zero-sample windows as zero.
- Timings are CPU-side **wall time** and may include blocking, preemption or
  driver waits. GXM, texture, and draw nested spans overlap; do not add them.
- NID summary data is **full-session only**. A report with
  `--start-ms/--end-ms` filters frame and hotspot windows but must still
  label the HLE NID table as full-session.
- Trace events contain GXM NewFrame commands, batch consumption, render-end,
  long-frame markers, shader pipeline events and JIT cache recycling.
  High-frequency HLE events are **not** emitted by default.
- `render_loop` is not simulated PSV FPS, independent unique game frames,
  or physical panel scanout. GPU execution timestamp is **unmeasured**.
- The collector's `collector_poll` metric now covers RAM aggregation only;
  output writes occur after measurement ends.

## PC analysis

Requires Node.js but no npm dependencies:

```sh
node scripts/analyze-switch-tracelab.mjs ./tracelab-PCSD00035-... \
  --start-ms 100000 --end-ms 210000 \
  --html report.html --trace-json events.json > report.json
```

The analyzer supports **old schema v1** traces as well, using contiguous
frame IDs to filter missing-event gaps. For v2 it reports pooled hotspot
estimates, independent frame interval statistics, aggregated NID samples
(full session), and thread CPU checkpoint data.

## First validation run

1. Build/deploy the updated debug NRO, using the same Vulkan/NVK backend.
2. Start with `mode=auto`, `legacy_csv=off` and LSFG disabled.
3. Warm the shader cache; measure 90–120 seconds in a repeatable heavy scene.
4. Exit the game normally and allow the TF card to finish saving.
5. Verify `frames_overflow=0`, `events_dropped=0` or near zero,
   `window_rows_overflow=0`, and that `frames.csv` IDs are continuous.
6. Check that `collector_poll` elapsed time is significantly lower than
   the earlier 175 ms sampled average from the SD-writing collector.
7. Re-run using `mode=off` (also with `legacy_csv=off`) to estimate the
   remaining TraceLab runtime impact on source-frame cadence.

Once the diagnostic fidelity is validated, examine `texture_hash_unchanged`,
`texture_hash_changed` and `texture_hash_scene_reused` alongside
`texture_hash` and `dcache_clean` bytes/time. A high unchanged ratio can
motivate a **separate** cache-version tracking experiment, but it does not
by itself make it safe to remove memory coherence operations.

This change does not include experimental GPU timestamps, asynchronous
network streaming, unsafe cache-skipping optimizations or new scheduler
affinity rules.
