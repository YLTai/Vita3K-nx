# Switch CPU Core 3 diagnostic profiling

This diagnostic build keeps the original thread priority, affinity, and
asynchronous shader worker count unchanged. It records actual Horizon CPU
running time for registered threads and separately records renderer wall time.

## Logs

Each game launch writes two CSV files on the SD card:

- `sdmc:/switch/vita3k/logs/cpu-threads-<title>-<epoch>-<run>.csv`
- `sdmc:/switch/vita3k/logs/render-phases-<title>-<epoch>-<run>.csv`

The complete paths are logged with a `[cpu-profile]` prefix in Vita3K's
regular log. The files are flushed roughly every three seconds and always
closed at a normal session exit. On an unclean crash, the last few seconds
may be lost.

## Thread data: cpu-threads CSV

One row is one active registered thread for the interval since its last
snapshot. `window_ms` is the measurement window for *that thread*.
`core0_ms` through `core3_ms` measure CPU time consumed on each core.
`coreN_pct = coreN_ms / window_ms * 100` is that thread's share of one
core in the interval, **not total system core utilization**.

`thread_id` is a monotonically increasing profiler registration ID, not
a kernel TID. Different shader-compiler workers get different IDs.
`priority` and `affinity_mask` reflect the initial state of each thread;
the profiler never modifies either. `state=exit` denotes the last sample
captured before the thread's libnx handle was destroyed.

Instrumented threads:

- `render`: GXM renderer and Vulkan recording/submission thread.
- `gxm-display`: guest display callback runner.
- `shader-compiler`: individual asynchronous shader pipeline worker.
- `vulkan-wait`: Vulkan fence wait/notification thread.
- `vblank`: Vita display vblank notifier.
- `switch-audio`: audren update worker.
- `preempt-watchdog`: guest thread preemption watchdog.
- `main`: Switch frontend loop.

Counter reads use `svcGetInfo(InfoType_ThreadTickCount, thread_handle,
TickCountInfo_CoreN)`. This requires Horizon 13+ and reads four counters
sequentially rather than atomically. A failed kernel query yields no row for
that sample. Threads not instrumented here, Horizon services, kernel work and
Mesa/NVK driver-internal threads are **not** included. Consequently, an
external monitor's Core 3 total can be higher than the sum of these rows.

## Render data: render-phases CSV

Every second, wall-clock duration of these renderer sections is aggregated:

- `process_batches`: GXM command-queue processing, including idle waits.
- `render_frame`: per-frame processing, possibly including Vulkan waits.
- `swap_window`: presentation path, potentially waiting for GPU/display.

`count`, `total_wall_ms`, `avg_wall_ms`, `max_wall_ms`, and counts
exceeding 16.7 ms and 33.3 ms reveal frame-time spikes. **Wall time is not
CPU execution time**, and these are not GPU hardware timestamps.

## Suggested measurement protocol

1. Start the same game at a reproducible in-game location.
2. Play 2-3 minutes at fixed CPU/GPU/RAM clocks and unchanged Vulkan settings.
3. Save both CSVs and note game/title ID, original rendering resolution,
   memory mapping mode, LSFG, and temperature.
4. Repeat with a warm shader cache; avoid mixing first-use compile spikes
   with warmed scenes.
5. Compare `render`, `gxm-display`, `shader-compiler` Core 3 percentages
   against the `render_frame`/`swap_window` wall-time spikes.

Do not conclude that a high CPU percentage alone proves a stall. A saturated
Core 3 can be caused by system processes or other non-registered threads;
use an independent core utilization monitor for a system-wide view.

Profiling is deliberately always on in this temporary diagnostic branch;
do not merge it into production releases without adding a user-facing toggle
or a compile-time opt-out.

## LSFG-specific diagnostics (v2)

The profiling build now writes **three** CSVs per title in the same log
directory. In addition to the two files above, it writes:

- `lsfg-state-<title>-<epoch>-<run>.csv`

The `render-phases` CSV preserves the original schema and adds these stages
(with one row per stage per second, including zero-count rows):

| Phase | What it actually times |
| --- | --- |
| `lsfg_record_compute` | CPU cost of recording frame-generation compute passes |
| `lsfg_capture_record` | CPU time recording original image capture |
| `lsfg_restore_record` | CPU time recording the restored original frame copy |
| `lsfg_queue_submit` | CPU time submitting LSFG command buffers (not GPU execution time) |
| `lsfg_present_generated` | CPU wall time inside the generated-frame QueuePresentKHR |
| `lsfg_present_original` | CPU wall time inside the original-frame QueuePresentKHR |
| `lsfg_acquire_image` | CPU wall time in AcquireNextImageKHR after presenting a generated frame |
| `lsfg_slot_fence_wait` | CPU wall time waiting for a reusable LSFG frame slot |
| `lsfg_create` | LSFG runtime initialization |
| `lsfg_destroy` | LSFG runtime destruction (includes possible DeviceWaitIdle) |

The `lsfg-state` CSV samples `requested`, `compatible`,
`high_fps_passthrough`, `source_present_calls`, `generated_frames`, and
`actually_generated`. A user requesting LSFG *does not prove it actually
generated frames*: use `generated_frames > 0` together with the stage counts.
This allows comparing LSFG Off/On within a recording without assuming the
toggle time from large swap-window spikes. Short changes within a one-second
sample may be coalesced; the file does not capture exact toggle timestamps.

The per-thread CSV also now contains raw per-core tick counters, the kernel
`TickCountInfo_Total` counter, return codes of both queries, and an
independent low-frequency total-tick reading made from inside selected
workers themselves (`self_*` columns). These fields test whether the original
near-zero thread CPU percentages were a kernel sampling/accounting artifact.
An absent self sample reports -1.0 percent, **not** 0% load. If kernel counters
still disagree with an external total-core meter, do not infer total CPU load
from these values until the discrepancy is understood.

All timings are CPU wall time or kernel CPU ticks, **not Vulkan GPU
timestamps**. The LSFG instrumentation changes neither the fence semantics,
command submission order, nor the number of shaders or presented frames.

For comparisons, capture at least 30 seconds LSFG Off, 30 seconds LSFG
generating frames, and another Off interval in a reproducible scene with
fixed clocks. Submit all three CSVs and the regular Vita3K log.
