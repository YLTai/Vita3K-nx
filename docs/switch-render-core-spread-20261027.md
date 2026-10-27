# Switch: render thread escapes saturated core 3

## Why this change

The PCSD00035 heavy-scene runs reached about 25 source-presents/s despite
roughly 2,450 draw commands per loop. The render thread is hard-pinned
to Horizon core 3 at priority 63. The user additionally observed
**core 3 nearly 100% busy at frame drops, while cores 0–2 stay below ~75%**.
Kernel per-core CPU columns in prior CSVs were internally inconsistent;
treat this separate hardware observation as a hypothesis about scheduler
contention, not as a verified profiler measurement.

A serial render thread cannot execute faster merely by having a larger CPU
mask. The opportunity is **less scheduling delay** when audio, display, NVK
helpers or the OS preempt its core-3 execution. Existing render CPU-total
samples suggest it does not consume the whole wall-clock frame.

## Single targeted change

With `-DSWITCH_RENDER_CORE_SPREAD=ON` (new default):

- Only the **render thread** gets a 0b1111 affinity mask, allowing Horizon
  to migrate it to otherwise idle application cores. The ideal core is
  `-1` (don't care) to let Horizon schedule the thread.
- **Set priority 63 before widening the CPU mask**, preserving priority
  precedence: Guest JIT threads have priorities 45–56; short audio/display/
  Vulkan helpers on core 3 have higher priority than the renderer. Rendering
  therefore runs only when those runnable threads yield that core.
- If changing priority or broadening affinity is refused, fall back to
  the original core-3 pinning policy.

With `-DSWITCH_RENDER_CORE_SPREAD=OFF`, the original render
`switch_pin_to_hardware_priority("render thread", 63)` is preserved.
This option does **not** affect thread count, guest core-mask semantics,
renderer commands, Vulkan queues, cache flushes, fences, synchronizations,
or visual work. It does not increase clock rates.

Potential downsides: migration and lost CPU cache locality; guest work
may fill remaining app-core time; if core 3 is busy solely with the render
thread and not its preemptors, there may be no speedup. Do not make a
performance claim without measuring the Switch.

## Hardware A/B

The dedicated PR's Actions build produces two NRO artifacts with the same
commit and Mesa SDK:

- regular `vita3k-switch-<SHA>`: render spread ON
- `vita3k-switch-render-core3-reference-<SHA>`: original core-3 render pin

Each artifact contains `Vita3K.nro`, `core/Vita3K.nro`, `build-info.txt`
and `SHA256SUMS`. Normal merged pushes build only ON.

Test both with the **same** PCSD00035 save and route, clocks, temperatures,
resolution, LSFG disabled, and graphics settings. Start from a warm cache.
Alternate ON/OFF in at least three pairs and compare:

1. Actual independent game frames and playback/audio speed.
2. `source_present` rate, P50/P95/P99 frame intervals, count >50/100ms.
3. `render` `self_core` in cpu-threads CSV (look for migration to 0–2
   only in ON), self/core-total CPU ticks, and `batch_command_execution`.
4. Draw/frame and SetState/frame: source work must remain comparable.
5. Sound glitches, emulator freezes, device loss, incorrect graphics.

Use complete heavy-scene windows rather than boot and shutdown.
Profiler's kernel per-core fields are not reliable in the existing data;
the self-reported current processor only proves where each checkpoint
occurred, not its full utilization distribution. A flat FPS result or
regression means this policy should be disabled or reverted.

**No real-game improvement figure is claimed by this PR.** Build success
only validates compilation and artifact creation; it is not a hardware test.
