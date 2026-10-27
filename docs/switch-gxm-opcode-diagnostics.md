# Switch / Vita3K: second-round GXM opcode diagnostics

The first A/B/C session with PCSD00035 showed ~25 source NewFrame/s
during high-load gameplay, with ~37-39 ms of `process_batches` wall time
and ~98% of its wall time in `batch_command_execution`. Those figures
**do not prove CPU execution**: GXM handlers may block on GXM sync, Vulkan
fences or driver calls.

This diagnostic change measures **which command handlers consume that
wall time**, without changing the emulator's rendering, guest thread
scheduling, queue submissions or LSFG implementation. LSFG initialization
stalls are outside scope.

## How to test

Use the CI artifact from the successful `ci/switch-build` merge run,
with its matching `build-info.txt`. Run using Vulkan/NVK with the same
PCSD00035 save, game route, shaders, clocks, scene and graphics settings
as the last three-run baseline.

**Minimum tests:**

1. **A: LSFG fully disabled** in the launcher, then launch the game.
2. **C: launcher LSFG prepared and enabled in the in-game quick menu**
   (L + R + Plus), then play the same scene.

The **B: prepared but Off** control is valuable when feasible; it separates
launcher swapchain setup from active frame generation. Prefer all three.
Do not mix the easy ~30 fps introduction with the later busy ~25 fps
section: warm up/advance into the high-load scene, then replay a
controlled **90-120 second** section for A and C. Repeat a second time
if feasible, keeping the same clocks and temperature conditions.
Avoid opening the quick menu in the measured section.

**After each run, exit the game normally** to finalize CSVs. If a crash
or graphical corruption happens, stop and send the available log/error
details without forcing more runs.

## Return five matching CSV files per run

Location: `sdmc:/switch/vita3k/logs/` (match title, epoch, session suffix).

| Filename | Purpose |
| --- | --- |
| `gxm-opcodes-PCSD00035-*.csv` | **NEW:** per-opcode counts, sampled handler wall times and observed maxima |
| `render-phases-PCSD00035-*.csv` | Existing phases plus nested GXM sync and Vulkan fence/scene wait phases |
| `frame-events-PCSD00035-*.csv` | GXM NewFrame/render cadence and sparse `gxm_handler_slow` events |
| `cpu-threads-PCSD00035-*.csv` | Guest/JIT and render thread total CPU ticks |
| `lsfg-state-PCSD00035-*.csv` | Verify source-present and generated counts and modes |

Send `build-info.txt`, A/B/C labels, actual CPU/GPU/RAM clocks,
docked/handheld, resolution, flow setting, cache status, and any stutter
or graphical artifacts. Keep filenames and original CSVs unchanged.

## Understanding the opcode CSV

Each row is a **1 Hz window** with:

- `calls`: exact number of handler invocations observed in that window.
- `sampled_calls`: handler executions actually timed.
- `sampled_wall_ms`: sum of **observed** timer durations.
- `estimated_total_wall_ms`: extrapolation of measured average duration
  over all `calls`. This is **not an exact sum** for sampled opcodes.
- `avg_sampled_wall_ms`, `max_sampled_wall_ms`: observed values.
  The maximum and threshold counts are from **only sampled calls**.
- `sampled_over_16_7ms`, `sampled_over_33_3ms`: observed sampled
  long handlers, not weighted estimates.

The high-frequency `Draw` and `SetState` handlers are timed on **one
of every 16 invocations**, to limit profiler overhead, but their calls
are always counted. All other known handlers are individually timed on
every call. Sampling starts from a persistent per-thread sequence, not
at the beginning of every frame. If `sampled_calls=0`, the estimated
time is reported as zero for that window; **do not interpret it as
zero actual execution time**. When comparing modes use multiple windows,
pooling sampled ticks and calls before estimating totals.

`frame-events` also includes sparse `gxm_handler_slow` events.
For their unsigned decimal `value`, the upper 32 bits contain
`opcode_id`, the lower 32 bits contain **measured microseconds**.
This permits time alignment with GXM NewFrame and render-loop delays.

### Additional render-phases rows

- `gxm_command_sync_wait`: `WaitSyncObject` handler's wait loop;
  **inside** `batch_command_execution` and the opcode time.
- `gxm_nop_completion_wait`: `Nop` handler's optional completion
  wait, including `wait_for_submitted_work`.
- `vk_submitted_fence_wait`: the actual Vulkan `waitForFences` calls
  in `wait_for_submitted_work` (nested in the above).
- `gxm_stop_recording`: `SyncSurfaceData` handler's Vulkan
  `stop_recording` call (nested in the `SyncSurfaceData` opcode).

**Never add nested/overlapping phase totals** to
`batch_command_execution` or to each other. Opcode handler totals also
omit inter-handler setup, linked-list traversal, command recycling,
outer queue waiting, and rendering/present after `process_batches`.

These counters measure **host wall time**, not individual CPU instruction
cycles, actual GPU execution or panel scanout. If GPU/driver execution
appears dominant, the next isolated change should capture asynchronous
GPU timestamps with delayed retrieval, not insert a per-frame GPU wait.

The new instrumentation itself can perturb frame time. A substantially
lower A-mode performance than the preceding diagnostic baseline is a
signal to reduce profiler sampling before trusting small speed deltas.
