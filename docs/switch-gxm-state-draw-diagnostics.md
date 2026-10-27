# Nintendo Switch: state-type and Vulkan Draw hot-path diagnostics (round 3)

This diagnostic builds on the two earlier profiler changes, without changing
emulator behavior or LSFG. Its purpose is to resolve the **PCSD00035** A-mode
high-load case: the previous `gxm-opcodes` report estimated that Draw and
SetState consumed most of `batch_command_execution` wall time. The older
fixed every-16th sample could alias with a repeated draw/state sequence; the
new finer-grain sampler uses a per-render-thread pseudorandom 1/16 decision.

## Run only A (LSFG completely disabled)

A single A-mode test is sufficient to identify which command-handler stages
consume wall time. B and C runs are **not needed for this round**.

1. Open the [Switch build GitHub Actions runs](https://github.com/YLTai/Vita3K-nx/actions/workflows/switch-build.yml).
   Download the **successful** merged `ci/switch-build` artifact and save
   `build-info.txt`. Deploy its top-level **Vita3K.nro**; do not accidentally
   launch the previously extracted/cached emulator core.
2. In the launcher choose **Vulkan/NVK** and **LSFG 2x = Off** (fully disabled).
3. Launch PCSD00035 from the **same save and graphics settings** as the last
   A-mode run. Warm up and reach the **busy ~25–27 GXM NewFrame/s scene**;
   do not use the easy ~30fps introductory period as the performance sample.
4. Reproduce the same inputs/camera route for **90–120 seconds** at the
   same actual CPU/GPU/RAM clocks, resolution, dock/handheld state, shader
   cache state and temperature as the previous diagnostic.
5. **Exit normally** to the launcher so the profiler drains its last buffers.
   Repeat once if possible. A/B/C comparisons are unnecessary now.

## Return these six matching CSVs

Directory: `sdmc:/switch/vita3k/logs/`. Do not rename or mix sessions;
all filenames for one test must share the same
`PCSD00035-<epoch>-<session>` suffix.

| Name | Use |
| --- | --- |
| **`gxm-state-draw-*.csv`** | **NEW, most important:** 18 GXM SetState subtypes and 7 Vulkan Draw stages, with exact counts and randomly sampled handler wall time |
| `gxm-opcodes-*.csv` | Previous top-level opcode statistics (fixed 1/16 for Draw/SetState) |
| `render-phases-*.csv` | Batch CPU wall time, queue and GXM sync waits, Vulkan submits and fences |
| `frame-events-*.csv` | Frame cadence, long sampled handlers and per-frame timing |
| `cpu-threads-*.csv` | Host render and guest/JIT CPU ticks |
| `lsfg-state-*.csv` | Verifies this session remained LSFG Off |

Please also supply `build-info.txt`, clocks, resolution and any
visible stutter, missing textures, audio problems or crashes. If crashes
occur, keep the ordinary app log and any partial CSVs.

## What the new columns mean

`gxm-state-draw` uses the columns:
`elapsed_ms,metric_id,metric,measurement,calls,sampled_calls,sampled_wall_ms,estimated_total_wall_ms,avg_sampled_wall_ms,max_sampled_wall_ms,sampled_over_16_7ms,sampled_over_33_3ms`.

- `calls` is the **exact number of times the subtype or stage was reached**
  in that window. If Draw skips early or does not refresh its pipeline,
  subsequent stage counts will be lower; **they are not missing samples**.
- `measurement=random_1_in_16`: stopwatch readings come from one
  pseudorandom ~1/16 subset, instead of the fixed every-16th command.
  `estimated_total_wall_ms` is a **statistical extrapolation** from the
  observed mean; it is not a precise additive CPU cost. Pool measurements
  from many 1Hz windows before ranking short handlers.
- `measurement=counter_only`: `draw_skipped_mask` and
  `draw_skipped_pipeline` are **counts, not timings**. Zero estimated
  time on these rows is expected and does **not** mean no draws were skipped.
- `sampled_calls=0`: there were no timed samples in that particular
  window. A zero estimate in that case **does not establish zero cost**.
- `draw_setup` spans initial pass/macroblock/barrier/query preparation.
  `draw_pipeline_lookup` spans the pipeline cache lookup/retrieval call;
  `draw_pipeline_bind` spans the rest of the refresh branch.
  `draw_uniforms` spans UBO preparation; `draw_descriptors` spans
  descriptor retrieval/update/bind; `draw_index_vertex` spans index and
  vertex data preparation/bind; `draw_emit` spans draw recording and
  post-draw surface bookkeeping. These segments are measured only for
  Draws reaching them.
- The Draw stage times are **nested inside** top-level Draw handler time
  (and hence `batch_command_execution`). Do not add top-level and child
  times to calculate frame time. **GPU execution time and panel scanout
  are not directly measured**.
- SetState subtypes use the source enum indices, including Texture,
  UniformBuffer, Program, VertexStream, Viewport and 13 more.

## Diagnostic interpretation and cautions

The earlier A run showed ~26.58 GXM new-frame commands per second from
90–150s, and an estimated ~84% Draw + SetState share of batch execution.
That percentage is **a sampled wall-time estimate**, not proven CPU
computational utilization or a promised optimization yield. The new
results should reveal whether a few SetState categories, Draw descriptors,
vertex/index handling or pipeline lookup dominate.

The profiler adds stopwatch reads for sampled commands and one local
counter update per category. Verify new A-mode FPS remains in range of
the previous A-mode runs before trusting marginal per-frame gains. If
the measured FPS changes materially without any renderer settings
change, report the difference and compare with the previous NRO.

**Never remove GPU fences, surface synchronization, texture refreshes,
Draw calls or guest scheduling to get an artificially high FPS.**
