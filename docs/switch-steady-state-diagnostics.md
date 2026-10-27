# Switch steady-state profiling (diagnostic build)

This branch only adds measurements. It does **not** modify LSFG algorithms,
thread priorities, Vulkan synchronization or GPU work. Startup/LSFG creation
stalls are outside this experiment's primary scope.

## Get the build

1. Open the GitHub Actions **Build Switch NRO** run for the merge commit on
   `ci/switch-build`, and download `vita3k-switch-<sha>`.
2. Install the included **Vita3K.nro** (launcher + emulator) using the same
   location and launch method you already use. Keep `build-info.txt`.
3. Do not accidentally run the older cached/extracted emulator. Verify the
   embedded core and the GitHub Actions build commit agree with `build-info.txt`.

## Three comparable modes

Use Vulkan/NVK, the **same game and save, same camera or input route, same
resolution, shader cache state, clock/power/thermal conditions, and game
settings**. Do not clear caches between runs unless testing cold cache. Make
each measured section **90–120 seconds** after loading and a warm-up; repeat
at least **three** times if practical.

- **A: completely disabled.** In the launcher, set
  **Frame generation > LSFG 2x (Vulkan only) = Off** and *relaunch* the game.
  Warm up for 30 s, then play the reproducible 90–120 s route.
- **B: prepared but Off.** In the launcher, set **LSFG 2x = On**, use
  **Flow resolution = Quarter** and **Performance mode = On**. Relaunch.
  Do *not* enable FG in the game's quick menu. Warm up and record the same
  route for 90–120 s.
- **C: actively generating.** With launcher LSFG still enabled, press
  **L + R + Plus** to open the in-game quick menu and enable LSFG. Ignore
  the initial enable/transient period (at least 20 s); then replay the same
  90–120 s route. Avoid opening menus during the measured section.

For best comparability, perform B and C in different launches from the same
save, or use A/B/C runs with precisely scripted camera/input. A single
unscripted Off->On walkthrough is **not** a controlled FPS benchmark.

After each run, **exit the game normally to the launcher** and copy its log
files. This permits final CSV flushing. If the emulator crashes, keep every
log that exists, including the main application log. Never rename a file until
you have copied the entire original session.

## Files to return

Directory: `sdmc:/switch/vita3k/logs/`

For **each** run send the five CSVs with the **same
`<title>-<epoch>-<session>` suffix**:

| Name | What it measures |
| --- | --- |
| `cpu-threads-*.csv` | Per-thread total kernel CPU tick deltas; new `guest-jit` rows. Treat per-core percent columns as unreliable on known Horizon kernels. |
| `render-phases-*.csv` | Existing render/LSFG summaries plus batch queue wait, sync wait, command-handler wall time, Vulkan queue lock/submit/present, swapchain acquire/fence, and synchronous one-time helper. |
| `lsfg-state-*.csv` | Requested, compatible, bypass and per-window source/generated counts. |
| `frame-events-*.csv` | Raw timestamped software events for renderer loop and GXM `NewFrame`, source-present calls, generated-return counts and sparse slow GXM handlers. |
| `gxm-opcodes-*.csv` | 1 Hz per-opcode counts and sampled handler wall time. Draw/SetState time is sampled 1/16, all other handlers are fully timed. See [round-two instructions](switch-gxm-opcode-diagnostics.md). |

Also send **`build-info.txt`**, the test's **A/B/C label**, exact
clock/power mode, handheld/docked, resolution, settings, cache status,
and any graphical defects, dropped frames or audio glitches. An ordinary
Switch application log is helpful if a stage reports errors or crashes.
A short matching capture/video is useful for actual display cadence.

## Understanding the new phases

- `process_batches` is an overall wall-time envelope, **not pure CPU work**.
- `batch_queue_wait` measures queue `top(3000)` waits.
- `batch_sync_wait` measures GXM sync-object readiness waits.
- `batch_command_execution` includes all command handlers and their
  Vulkan/driver waits; nested GPU/queue phases **overlap** it. Do not sum
  their totals to calculate frame time.
- `vk_queue_mutex_wait` measures time acquiring the shared Vulkan mutex.
- `vk_queue_submit` measures the **host** submit call, not GPU execution.
- `vk_queue_present` includes the entire LSFG present bridge when active;
  the LSFG sub-stages are *inside* this wall-time envelope.
- `swapchain_acquire` and `swapchain_fence_wait` measure host blocking at
  these points, not necessarily GPU processing time.
- `vk_one_time_submit_wait` includes synchronous one-time commands and
  any fence/GPU wait performed by that helper.
- `gxm_new_frame` is **a GXM command seen by the renderer**, not independent
  proof of a unique simulated frame; the actual game can submit repeated
  display buffers or skip GXM commands.
- `source_present` marks entering the Switch LSFG present wrapper.
  The integer value is a bitfield: 1=requested, 2=compatible,
  4=high-FPS bypass. A value of 0 is not an error.
- `generated_return` counts extra presents returned by the LSFG bridge,
  not physical display scanout timestamps.
- `render_loop_begin`, `batches_done`, `render_frame_done`,
  `render_swap_done` delimit the renderer software loop.
- `swapchain_acquire_timeout` indicates an unsuccessful image acquire.
- `events_dropped` is a **diagnostic failure**: the bounded 8192-event
  ring overflowed before the frontend drained it. Repeat with a stable
  game/foreground session; do not report exact percentiles for such a run.

`elapsed_ms` uses the same per-session system-tick origin in all five CSVs.
For `frame-events` this is **event time**, whereas the other CSVs are 1 Hz
window samples. The `sequence` field records the order in which events
entered the buffer; cross-thread timestamps can have tiny ordering changes.

## What this experiment can and cannot prove

It helps separate queue starvation, expensive command handling, guest/JIT CPU
saturation, host queue lock/contention and presentation backpressure. It can
measure raw **software** frame intervals rather than relying on a 1 Hz average.
It cannot directly measure GPU shader/kernel execution or actual panel scanout:
this patch deliberately avoids synchronous GPU timestamp waits that would
modify the timing under investigation. If evidence indicates a GPU-bound case,
the next isolated patch should add asynchronous Vulkan query-pool timestamps
with delayed retrieval and a CPU/driver baseline comparison.

The new guest-thread tick sampler adds some 1 Hz kernel queries and the
timestamp ring adds lightweight mutex operations. These can perturb a busy
Switch. Reproducibility across repeat runs is essential; do not infer 1% FPS
improvements from noise.

Stop and report the problem instead of continuing the benchmark if the new
build crashes, displays corrupted textures, loses input/audio, or shows severe
regressions. Keep the prior known-good NRO available for recovery.
