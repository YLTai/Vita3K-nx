# Switch performance evidence and optimization review (2026-10-27)

## Evidence

### Scope and provenance

The source review starts at `ci/switch-build` commit
`7b1cdce3e6af2be1b5d01f2777384de65f972b09`. The six supplied files share
the suffix `PCSD00035-1793102688-1(1).csv`. They do **not** include
`build-info.txt`, a source commit, clocks, temperature, resolution, or a
recorded input route. Their schema matches the reviewed profiler, but this
does not establish that this commit produced the recording. These are
baseline observations, not measurements of the changes in this review.
The original CSVs are not committed to this repository.

| CSV prefix | Data rows | Coverage |
| --- | ---: | --- |
| `cpu-threads` | 7,191 | 221 sampling windows |
| `render-phases` | 5,746 | 221 windows, 26 phases |
| `gxm-opcodes` | 4,420 | 221 windows, 20 opcodes |
| `gxm-state-draw` | 5,967 | 221 windows, 27 metrics |
| `lsfg-state` | 221 | 221 sampling windows |
| `frame-events` | 32,727 | 984.550 to 222,663.300 ms |

The final aggregate sample is at 224,239.87 ms. Event sequence numbers are
continuous, timestamps do not go backwards, and there are no
`events_dropped` records. All 5,651 `source_present` events reconcile with
the sum of `source_present_calls`. LSFG requested/compatible/bypass and
generated-frame fields are zero throughout. Both `draw_skipped_mask` and
`draw_skipped_pipeline` counters are zero throughout.

### Select the sustained workload before ranking bottlenecks

The early recording includes startup and low-command-volume periods.
Some early render loops time out after about 500 ms waiting for work.
From approximately 80 seconds onward, command execution becomes sustained
and queue waiting becomes small. The analysis retains the slow frames in
that sustained workload and excludes startup and exit activity.

Use the common sampling boundaries **(80,682.87, 219,860.19] ms**: 138
complete windows lasting **139.17732 seconds**. This is the inward-rounded
80-to-220-second selection. It contains 3,517 render loops, 3,517 GXM
NewFrame events, and 3,517 source presents. Each complete software loop
beginning in the selection has one NewFrame event.

`source_present` marks entry into the LSFG present wrapper, before the
actual present. `gxm_new_frame` marks execution of the renderer command.
Their correspondence supports comparing software frame throughput, but
neither is a panel scanout timestamp or proof of a unique simulation
frame. Actual game speed and visible output must also be checked on hardware.

| Sustained source-present cadence | Result |
| --- | ---: |
| Count / selected duration | 25.26992 presents/s |
| Mean adjacent interval (3,516 intervals) | 39.57406 ms |
| P50 / P95 / P99 | 38.65500 / 43.55975 / 56.53155 ms |
| Maximum interval | 125.82600 ms |
| Intervals over 50 / 100 ms | 44 / 4 |

Percentiles use linear interpolation at `(N - 1) * p` in sorted interval
samples. Only adjacent events both inside the selection are included.
Dividing interval count by their total duration gives 25.26908/s; the
small difference from the count/duration result is endpoint handling.
One-second window counts must not be averaged and labeled FPS because
these sampling windows are not exactly one second long.

### The renderer command path dominates sustained wall time

The following values divide the selected aggregate totals by 3,517 loops.
Wall-time phases include blocking and descheduling. They are not GPU
timestamps or pure CPU execution time. Parent and child phases overlap;
**do not add this table's rows together**.

| Phase | ms per render loop | Relationship |
| --- | ---: | --- |
| `process_batches` | 38.53502 | Parent of command execution and queue waiting |
| `batch_command_execution` | 37.83974 | 95.62% of the selected elapsed interval |
| `batch_queue_wait` | 0.65415 | Within `process_batches` |
| `render_frame` | 0.41465 | Final output preparation |
| `swap_window` | 0.55112 | Includes the present path |
| `vk_queue_submit` | 0.90895 | Nested host submit calls; 19 calls/loop |
| `swapchain_fence_wait` | 0.02971 | Host fence wait |
| `gxm_command_sync_wait` | 0.00387 | Nested GXM synchronization |
| `vk_queue_mutex_wait` | 0.00187 | Host queue lock acquisition |

Whole-session queue wait is 64,111.519 ms, but only 2,300.658 ms falls in
the sustained selection. Treating the whole-session figure as removable
steady-state overhead would misidentify the bottleneck. The measured
sync/queue waits do not justify weakening synchronization. Small host
submit and fence times also do not prove that the GPU is idle: GPU work
can overlap these software phases.

### Fine sampling identifies the most valuable work to inspect

The profiler computes each window's estimate as
`calls * sampled_wall_ms / sampled_calls` before CSV rounding. The table
below sums the CSV estimates and divides by the loop count. Fine metrics
use pseudorandom sampling at approximately 1/16. The outer Draw/SetState
opcode sampler is fixed every 16 calls and can alias repeated workloads.
Counters are exact for the instrumented paths; estimates are not exact
CPU costs, and instrumentation itself can perturb short handlers.

| Fine metric | Calls/loop | Estimated ms/loop |
| --- | ---: | ---: |
| `state_texture` | 3,260.30 | 14.01152 |
| `draw_index_vertex` | 2,463.16 | 6.71762 |
| `draw_descriptors` | 2,463.16 | 5.30994 |
| `draw_uniforms` | 2,463.16 | 2.49880 |
| `draw_emit` | 2,463.16 | 1.79478 |
| `state_uniform_buffer` | 6,092.73 | 1.42436 |

The selection executes 8,662,948 Draw commands and 46,276,098 SetState
commands, with nine SyncSurfaceData calls per loop. Pipeline lookup and
pipeline binding are much smaller: approximately 0.05973 and 0.25881 ms
per loop. Shader-compiler threads record zero kernel CPU ticks in this
selection. Texture handling, index/vertex preparation, and descriptor
handling are therefore stronger optimization candidates than pipeline
compilation. Their measured cost is **not** a promised saving: only
redundant work proven unnecessary by the source can be removed.

Fine stages are nested inside Draw/SetState, which are nested inside batch
execution. Adding these levels double-counts time. Independently sampled
phase counters and rounded CSV values need not reconcile exactly at an
individual window boundary.

### CPU counters and scheduling limits

The four per-core millisecond columns sum to only **3.988 ms** over the
entire recording, whereas kernel total tick deltas imply
**464,254.057 ms** of CPU time. Both query return codes report success.
This internal inconsistency means the per-core percentages cannot be used
as utilization measurements for this recording.

Use `delta_total_ticks / tick_frequency * 1000` and divide summed CPU
milliseconds by the common selected duration. The render thread consumes
**68.8146% of one core**, and all guest-JIT threads together consume
**181.1003%**, or 1.811 core equivalents. Guest threads must be aggregated
against one common interval, not against the sum of their lifetimes.
Render runs at priority 63 with affinity `0x8`; its independent self-query
reports core 3 and averages 68.7235%. Self samples have different checkpoint
windows, so this is corroboration rather than an identical measurement.

The profiler's helper threads each use less than 0.5% of a core in this
selection. These results do not measure unregistered Horizon/driver
threads, runnable-but-descheduled time, or GPU utilization. They do not
justify changing guest affinity, moving sustained rendering to a higher
core-3 priority, or claiming that all application cores are saturated.

### Profiling can contribute to long-tail stalls

In the reviewed baseline, `SwitchCpuProfileSession::poll()` holds
`cpu_profile_mutex` through CSV writes and periodic flushes. The renderer's
once-per-second `checkpoint()` takes the same mutex after
`render_swap_done`. This is a concrete source-level blocking path.

For complete loops beginning in the sustained selection, the gap from
`render_swap_done` to the next `render_loop_begin` has median **0.001 ms**
and P99 **0.005 ms**. Five exceptional gaps of **64.648, 79.735, 22.801,
52.365, and 34.634 ms** follow sample polls near 124.046, 125.048, 126.052,
127.059, and 129.076 seconds. Together they account for 254.183 ms.
The timing correlation and shared lock support a profiling-induced-stall
hypothesis; the CSV does not directly record mutex waiting, so it cannot
prove each individual stall's cause.

Those five gaps represent only about 0.18% of the selected duration.
Avoiding them targets tail latency, not a large average FPS increase.
Other outliers remain: the largest batch phase is 125.125 ms and one
swap takes 58.961 ms. A profiler-lock fix must not be presented as an
explanation or cure for every slow frame.

### Reproduce the evidence

Place the six unchanged, matching CSV files in one directory, then run:

```sh
node scripts/analyze-switch-performance.mjs /path/to/session \
  --start-ms 80000 --end-ms 220000 > baseline-analysis.json
```

The dependency-free script reads inputs only, rejects mixed sessions,
checks common sample boundaries, rounds the requested selection inward,
and emits JSON containing file hashes, cadence, phase totals, sampled
hotspots, kernel CPU usage, skipped-draw counts, LSFG state, and post-swap
gaps. Its output is an analysis of recorded software events, not evidence
of a new build's speed or visual correctness. Use the same scene and
measurement settings for paired hardware runs and retain `build-info.txt`.
The current Switch frontend enables the profiling session at launch;
there is no profiling-off launcher control. Assessing the observer effect
requires separate, equivalently configured diagnostic-disabled control
builds for both baseline and optimized code, not an assumed UI toggle.

## Changes and correctness conditions

### Avoid redundant Vulkan descriptor bindings

`draw_bind_descriptors()` previously bound all four graphics descriptor
sets and all dynamic offsets on every draw. It now tracks the last bound
sets and active offsets for the current command-buffer recording. Identical
state needs no bind. Changes use one contiguous range from the first to
the last changed set, avoiding the fixed cost of several separate calls.

The cache is deliberately conservative: a pipeline-layout handle change
forces a full four-set bind. Every `start_recording()` invalidates it,
including scene splits and reuse of the same command-buffer handle. Dynamic
offsets belong only to set 0 and are supplied only when that set is bound.
Texture descriptor allocation, writes, image-content checks and all draws
remain in place. Image layout is also included in `sync_texture()`'s
descriptor invalidation condition, alongside sampler and image view.

The Vulkan specification's [pipeline layout compatibility rules](https://docs.vulkan.org/spec/latest/chapters/descriptorsets.html#descriptorsets-compatibility)
permit retaining unaffected bindings with the identical layout. The
conservative full rebind on a layout change avoids relying on prefix
compatibility between different layouts.

This targets part of the estimated 5.31 ms/loop descriptor stage. The
recording has no histogram of identical descriptor snapshots, so it does
not establish a hit rate or a millisecond/FPS saving.

### Precompute immutable vertex-input metadata

The shader patcher copies vertex attributes before creating a renderer
program. After shader register analysis and its missing-symbol fallback,
creation now computes the used stream count and the maximum effective
attribute offset, including register-format arrays. Draws and pipeline
input checks reuse these values instead of repeatedly traversing the
attribute list and register map. The asynchronous compiler copies this
metadata together with the program.

The device-specific comparison still uses the current physical device's
`maxVertexInputAttributeOffset`. Index scans, vertex/index buffer binding,
guest-data checks, uploads, cache maintenance, and draws are unchanged.
This removes two immutable-metadata scans per draw from the vertex path;
it does not eliminate the full estimated 6.72 ms/loop index/vertex stage.

### Fast-path the most recently used sampler

After constructing the existing complete sampler key, the texture cache
first compares it with its LRU queue's MRU entry. A match directly returns
the existing index. All valid keys are nonzero, whereas unused entries
have key zero. A miss follows the original hash lookup and eviction path.
The key still contains filtering, addressing, LOD and force-nearest state.

This introduces no second cache or new invalidation lifetime. Texture
hashing, dirty detection, surface aliases, uploads and image transitions
still execute. It targets a small repeated operation within the largest
measured stage; the logs do not isolate sampler cost or MRU hit frequency.
More aggressive caching of `sync_texture()` results was rejected because
the required surface/content checks must remain valid after GPU writes,
guest writes and scene transitions.

### Keep diagnostic disk I/O off the worker checkpoint path

`SwitchCpuProfileThread::checkpoint()` now attempts the profiling mutex
without blocking. If the frontend is writing CSVs, the worker returns and
retries at its next checkpoint; it does not advance its deadline. A
successful attempt checks the session/token and samples the actual CPU
and wall ticks. Delayed self samples therefore retain their real elapsed
interval. The independent kernel-total samples remain available.

This removes the identified lock-wait mechanism from render/audio/helper
workers. Its expected benefit is fewer diagnostic-induced tail stalls.
It cannot explain all slow frames, and the correlated gaps account for
only about 0.18% of the supplied sustained recording. A poll may have no
new self sample while the lock is busy; that must remain distinguishable
from zero CPU usage.

## Review of scheduling and the actual driver

The review covered the command producer/consumer and synchronization
paths, guest thread priority and affinity handling, Dynarmic scheduling
integration, Switch pthread TLS, Vulkan recording/submission/wait threads,
texture/surface caches, descriptors, pipelines, buffer uploads, and LSFG
presentation. No affinity or priority changes are justified by these
samples. The per-key pthread TLS mutex is a possible future profiling
target, but its steady-state cost is not measured here.

The build uses [Mesa Switch revision efaf009f454e35fc6bcf4357e52455e30bfc3356](https://github.com/danfromtico/mesa-switch/tree/efaf009f454e35fc6bcf4357e52455e30bfc3356),
version 26.2.1, built from source with the repository's three CI portability
patches. This review does not update or patch that driver.

| Fixed-revision driver implementation | Finding relevant to this change |
| --- | --- |
| `src/nouveau/vulkan/nvk_cmd_buffer.c`, `nvk_bind_descriptor_sets` | Processes bound sets/dynamic offsets, writes root arrays and marks the supplied sets' cbufs dirty. |
| `src/nouveau/vulkan/nvk_cmd_buffer.h`, root-array setters; `nvk_cmd_draw.c`, `nvk_cmd_flush_gfx_root_desc` | No equality test before root flush; GM20B records constant-buffer update commands. Avoiding redundant binds removes actual recording and command data. |
| `src/nouveau/vulkan/nvk_cmd_draw.c`, index/vertex binding | Records binding/MME state. This change retains index and vertex binding calls. |
| `src/nouveau/vulkan/nvk_queue.c`; `nvkmd/switch/nvkmd_switch_dev.c` | Submission flushes heaps, waits for uploads/input synchronization, submits work, and signals completion. Native fences can become GPU waits; fallback host waits are required. |
| `src/nouveau/horizon/nouveau_horizon_channel.c` | In-flight throttling, command storage lifetime and CPU-visible completion fences are correctness requirements. |
| `src/nouveau/horizon/nouveau_horizon_memory.c`; `nvk_device_memory.c` | Cache clean/invalidate implements CPU/GPU visibility and must be preserved. |
| `src/vulkan/wsi/wsi_common_switch.c` | Native multifences are passed to the compositor where supported; the fallback waits before presenting an unfinished image. |

The supplied counters do not include asynchronous GPU timestamps, GPU
busy, or time spent inside individual driver functions. A GPU bottleneck,
core-3 descheduling, and specific texture substage costs remain possible
follow-up questions. No GPU fence, semaphore, GXM completion, cache flush,
render pass or draw is removed by these changes. LSFG behavior is unchanged.

The baseline already permits asynchronous-pipeline draw skipping on
Switch, including whole-screen quads. This review does not extend that
behavior. The supplied recording reports zero skipped draws. Cold-cache
or mismatched skipped-draw runs must not be used to claim a speedup.

## Validation and hardware acceptance

Run the focused host regressions with:

```sh
git submodule update --init external/boost
bash scripts/tests/run-switch-performance-tests.sh
```

The descriptor replay compares emitted ranges against full binding across
every changed-set/offset mask in both mapping modes, plus 10,000 state,
layout and recording-reset transitions. Vertex metadata is checked against
independent enumeration for boundaries and 10,000 generated cases. Sampler
recency/index behavior is compared with a list model for 20,000 accesses
at each of five capacities. The checkpoint test compiles the production
method with a fake clock/kernel API, verifies nonblocking contention,
immediate retry and true elapsed-span accounting, and fails against the
original blocking implementation. These host checks passed locally.

The Switch workflow also runs these checks before cross-compilation. Its
new `pull_request` trigger targets `ci/switch-build`, allowing the NRO build
to be checked before merge; the existing push trigger still builds the
merged branch. Host tests and compilation do not validate real GPU output.
No post-change Switch FPS or frame-time measurement is available yet.

For hardware acceptance:

1. Retain both NROs and their `build-info.txt`/hashes. Use the same
   Vulkan/NVK driver revision, PCSD00035 save, busy-scene route, resolution,
   graphics options, actual CPU/GPU/RAM clocks, handheld/docked mode,
   temperature range, SD card and warmed shader cache. Keep LSFG fully off
   for the primary source-frame comparison.
2. Run at least three matched baseline/optimized pairs, alternate their
   order, and measure the same 90–120 second route after warmup. Keep the
   profiler configuration identical. Do not combine startup and steady
   gameplay, and do not discard slow frames inside the selected route.
3. Compare source/NewFrame/loop throughput, mean/P50/P95/P99/max intervals,
   counts above 50/100 ms, kernel-total CPU ticks, and per-source-frame
   texture/index/descriptor costs. Inspect post-swap gaps around log polls.
   Use per-run results and their variation rather than only one pooled mean.
4. Check matching game progress, visible unique frames, audio speed and
   input response with an external recording. Verify scene transitions,
   render-to-texture effects, depth/stencil, postprocessing, texture filter
   changes, instancing, menus, pause/resume and normal exit. Where available,
   use desktop Vulkan validation with both mapping modes and frequent
   pipeline/scene changes to check descriptor lifetime/layout invariants.
5. Confirm skipped-draw counts do not increase and workload draw/scene
   counts remain comparable. Reject apparent gains caused by missing
   geometry, stale textures, visual corruption, altered simulation speed,
   GPU errors or newly skipped work. Test LSFG prepared/off and active modes
   separately afterward; generated output frames are not source FPS.

Accept a performance gain only when repeatable hardware results exceed
run-to-run variation and all correctness checks pass. If a regression is
isolated, revert that optimization or the PR; do not compensate by weakening
rendering or synchronization. The current deliverable establishes removed
redundant work and host correctness checks, not a numerical FPS promise.
