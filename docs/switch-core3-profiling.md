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
