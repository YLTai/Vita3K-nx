# Vita3K-nx TraceLab diagnostic build

This is an **instrumented** Switch NRO for determining where heavy 3D
scenes spend their time. It **does not** change CPU scheduling, renderer
correctness, memory cache rules, or GPU command ordering. The
`ci/switch-build` workflow now produces one NRO artifact containing both
the launcher and instrumented core; the retired descriptor-cache and
core-3-only A/B reference builds have been removed.

The build configures `-DSWITCH_TRACELAB=ON`. For an ordinary local
Switch build, TraceLab is **OFF** unless enabled explicitly. Legacy
diagnostics remain available to make comparisons with earlier recordings.

## Run

1. Install the resulting single top-level `Vita3K.nro` artifact as before.
   The launcher bundles an emulator; do not accidentally run a stale core
   extracted from an older launcher.
2. Optionally create `sdmc:/switch/vita3k/tracelab.cfg` with the contents:

   ```ini
   # Read once when each game session starts. This file is separate from
   # config.yml, which the launcher rewrites.
   mode=auto
   ```

3. Launch the same title/save and move to the reproducible heavy scene.
   Disable LSFG when profiling native/source-frame performance.
4. After shader warm-up, record a fixed scene for 90–120 seconds and **exit
   normally** so the last buffers and summary are written.
5. Copy the new session directory from
   `sdmc:/switch/vita3k/logs/tracelab-<title>-<epoch>-<run>/`.
   The older `cpu-threads`, `render-phases`, `gxm-state-draw`, etc.
   CSV files remain in `sdmc:/switch/vita3k/logs/`.

**No source edit or rebuild is needed to change diagnostic modes**;
edit the .cfg file before launching the next game session.

| Mode | Data collected |
| --- | --- |
| `off` | Disable new TraceLab counters and trace (older profilers are separate) |
| `light` | Lightweight counter/event telemetry; no sampled stage stopwatch |
| `cpu` | Guest JIT, high-frequency HLE and scheduler stopwatch sampling |
| `render` | GXM, texture, vertex/index, descriptor, pipeline and submit sampling |
| `auto` | **Default**: CPU + render sampled timing, frame events and long-frame markers |
| `deep` | Like auto but samples approximately 1/4 instead of 1/16; higher overhead |

The sampler uses a per-thread pseudorandom sequence. Stage calls are counted
exactly *within available slots*; sampled wall time and extrapolations are
**estimates**, not proof of CPU execution time. Do not add nested stages.

## TraceLab output

```text
tracelab-<title>-<epoch>-<run>/
  hotspots.csv   # 1 Hz per-thread-group counters and sampled wall time
  frames.csv     # render_loop completion timestamps and adjacent intervals
  trace.bin      # bounded binary cross-thread events, max 64 MiB
  manifest.json  # source title, mode, timebase, dropped-event count, context
  summary.json   # full-session totals for all named metrics
```

`hotspots.csv` columns:

- `elapsed_ms`: since this TraceLab session began
- `thread_slot`: TraceLab producer slot, **not** a kernel/guest TID
- `metric`: stable diagnostic metric name
- `calls`, `bytes`: exact counter totals for the interval if the slot
  is registered (byte counts are approximate source/work ranges for some stages)
- `sampled_calls`: stopwatch samples contributing to the following durations
- `sampled_wall_ms`: total elapsed **wall-clock** for the measured subset
- `estimated_wall_ms`: sampled wall sum scaled by calls/sample count;
  per-metric extrapolation only, **not additive**
- `max_sampled_us`: maximum sampled elapsed time

`frames.csv` records the render loop’s completion cadence. It is **not**
PS Vita simulated FPS, independent GXM frame count, Vulkan GPU execution time,
or physical display scanout. The existing `frame-events` and `lsfg-state`
logs help distinguish these events. TraceLab marks source-side long render
intervals of at least 66 ms.

The binary `trace.bin` begins with `TLABv1\0\0`, little-endian schema
version 1, record size (24), and the ARM counter frequency. Each record has
`tick:u64, correlation:u32, value:u32, kind:u16, thread_slot:u16, reserved:u32`.
Event types: render begin/end, GXM new-frame command, batch consumed,
sampled HLE NID+elapsed microseconds, 66ms render-loop stall, shader
pipeline compiled, JIT cache recycled. The buffer is bounded, never
holds a file lock in a hot thread, and reports dropped events. The trace
records do **not** pair GXM scenes and GPU submissions one-to-one.

The 1 Hz collector performs all SD writing on the Switch frontend/main
thread; renderer, shader and guest threads only update per-producer counters
and append to single-producer bounded rings. The `collector_poll` metric
tracks sampled collector wall cost. Dropped-event counts, trace size and
missing collector snapshots should be used to assess profiler quality.

## Analysis on PC

Requires Node.js (no npm packages):

```sh
node scripts/analyze-switch-tracelab.mjs ./tracelab-PCSD00035-... \
  --start-ms 100000 --end-ms 210000 \
  --html report.html --trace-json events.json > report.json
```

`report.json` ranks per-metric sampled wall-cost estimates, includes
the render-loop P50/P95/P99 intervals and records trace quality flags.
The optional `events.json` can be opened in Chrome tracing-compatible
viewers; the trace converter uses a common CPU clock origin and preserves
the thread-slot ID.

**Data integrity:** The generated logs do not prove GPU inactivity. GPU
timestamps are explicitly marked `unmeasured` until the NVK queue-families’
timestamp validity and asynchronous query retrieval are validated. The
existing Horizon per-core CPU tick reports can disagree with kernel total
ticks; treat them cautiously. Use the existing `cpu-threads` total tick/self
checkpoint reports for CPU-run-time context, never subtract them from wall
time and label the remainder pure scheduler waiting.

## Reproducibility / debugging limitations

- Keep the same title, save, scene/camera route, resolution, clock, driver,
  thermal/power mode, shader cache warmness and LSFG setting for A/B tests.
- Compare **the same TraceLab mode** (especially not `deep` against `off`).
  A separate `off` recording within the same instrumented build helps
  quantify overhead. Release+LTO remains necessary for final FPS decisions.
- TraceLab is not a complete guest PC sampler, Vulkan GPU timer, fault-based
  page-write tracker or kernel scheduler trace. It supplies correlations and
  actionable counters; a measured hotspot may still block in a driver call.
- High-frequency HLE samples store NIDs, not addresses or raw payloads. 
  No per-Draw text logging or SD writes are performed in the hot path.
- Abnormal exits can leave incomplete files or lose the most recent
  unflushed rows. There is a maximum of 64 producer slots and 64 MiB
  trace payload per session; excess data is counted as dropped.
- For deeper Vulkan profiling, a separate timestamp experiment is needed;
  do not claim a GPU utilization percentage from these diagnostics.

See `docs/switch-steady-state-diagnostics.md` for meanings of the legacy
six CSV files.
