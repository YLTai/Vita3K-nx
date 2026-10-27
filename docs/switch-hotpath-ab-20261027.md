# Switch render hot-path diagnosis and descriptor cache A/B

This change collects evidence before attempting another optimization. It changes
**no texture validity rules, barriers, uploads, fences, draw counts, render target
contents, GPU scheduling, or thread priority.** The descriptor-cache A/B choice
changes only whether redundant Vulkan descriptor-set *bind commands* are omitted.

## Motivation (two PCSD00035 sessions)

Two user-supplied six-CSV runs were compared using common complete sample
windows in the heavy scene (roughly 100–210 seconds). Software source-present
cadence was about 25.251/s versus 25.363/s (not evidence of FPS improvement),
while P99 present-event spacing was about 60.816 versus 46.712 ms.
The new run's `draw_descriptors` fine estimate increased slightly (about
5.310 to 5.503 ms/render loop) despite fewer descriptor bind calls being a
plausible source-level optimization. The cause remains unproven.

The current heavier-scene per-loop estimates are ~13.63 ms texture state,
~6.49 ms index/vertex, and ~5.50 ms descriptor work, but estimates are sampled
**CPU-side wall-clock spans**, with potential preemption, and are not all
recoverable time. A 25.3 to 30 Hz improvement would require reducing about
6.2 ms of elapsed time per displayed source frame. **Do not sum nested stages.**

The CSVs do not measure GPU execution time or actual scanout. This PR **does
not** add GPU timestamp queries: GPU timestamp validity, queue availability,
and readback overhead need device-specific validation before such numbers can
be trusted. Do not claim the system is CPU-bound or GPU-bound from these logs.

## Instrumentation

The existing per-thread Switch fine profiler records exact call counts in
thread-local state and samples timed sub-stages with a random ~1/16 decision.
**The extra hot-path instrumentation is compile-time OFF by default** to avoid
increasing ordinary Switch release overhead; set
`SWITCH_RENDER_HOTPATH_DIAGNOSTICS=ON` to collect the new rows. Both paired
artifacts from this PR's GitHub Actions job enable it. Merged push builds do not.
No counters or timing overhead from the new sub-stages runs when the option is OFF.
Its existing once-per-second frontend poll writes the CSV. This PR appends
new named metrics to `gxm-state-draw-*.csv` without changing column names.

Timed sub-stages (measurement=`random_1_in_16`):

| Metric | What the timed CPU-side span covers |
| --- | --- |
| `texture_surface_lookup` | Color-surface lookup, optional depth/stencil lookup, and nearby texture-key work |
| `texture_cache_operation` | Sampler binding on a surface hit or full texture-cache operation on a miss |
| `texture_descriptor_state` | Texture descriptor comparison, pipeline hints/UBO cast and viewport state |
| `draw_descriptor_resolve` | Texture set reuse/refresh and on-demand descriptor pool growth |
| `draw_descriptor_update` | Texture descriptor writes and driver update calls |
| `draw_descriptor_bind` | Cache comparison plus Vulkan bind submission, if required |
| `draw_index_data` | Index mapping/trap, optional max scan, bind or upload |
| `draw_vertex_data` | Vertex stream sizing/trapping and bind/upload |

All other new metrics are **exact counters**, with
`measurement=counter_only`; their `estimated_total_wall_ms` must remain zero.
The counter rows are not additional Draw or SetState operations.

Descriptor bind outcomes are mutually exclusive per completed descriptor path:
`desc_bind_skipped`, `desc_bind_partial`, `desc_bind_full` (cache enabled)
or `desc_bind_unconditional` (cache disabled). Exact counters additionally
track descriptor set refreshes, pool grows, update API calls, surface lookup
hits/fallbacks, descriptor changes/reuses, and index max-range rescans/reuses.
The last two cover only the memory-mapped/trapped index-buffer path; not
every draw accesses that path. Texture lookup counters exclude a null-palette
early return.

`scripts/analyze-switch-performance.mjs` now outputs `fine_counters` and
`hotpath_diagnostics` alongside the original values. It treats old log files
without these rows as unavailable and returns `skipped_pct: null`, not 0%.

## How to build and compare

The default Switch NRO is built with the existing descriptor bind cache:

```sh
cmake -S . -B build-switch -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$DEVKITPRO/cmake/Switch.cmake" \
  -DCMAKE_BUILD_TYPE=Release \
  -DSWITCH_DESCRIPTOR_BIND_CACHE=ON \
  -DSWITCH_RENDER_HOTPATH_DIAGNOSTICS=ON \
  -DSWITCH_VULKAN_SDK="$SWITCH_VULKAN_SDK" \
  -DSWITCH_OPENSSL_DIR="$SWITCH_OPENSSL_DIR"
cmake --build build-switch --parallel 2
```

The PR #7 workflow produces two identically instrumented artifacts:
`vita3k-switch-<SHA>` (cache ON) and
`vita3k-switch-descriptor-cache-off-<SHA>` (cache OFF). Both contain launcher
and core NRO files, checksums and `build-info.txt`. The OFF artifact is a
reference for this experiment and is not an optimized build. This optional
second build step runs only on this PR branch, not on ordinary merged pushes.

Build the reference with **the same commit, toolchain, dependencies and
settings**, but `-DSWITCH_DESCRIPTOR_BIND_CACHE=OFF`; use separate
build directories/artifact identifiers to avoid mixing variants. The OFF
variant executes unconditional `bindDescriptorSets` with all four sets and
valid dynamic offsets. Descriptor set allocation and updates are unchanged.
A/B is *compile-time*; it is not a runtime setting. Distinguish builds using
the compiled CMake cache and `desc_bind_unconditional` counter (nonzero in
OFF only). The OFF variant is only a reference and should not be merged as
an alleged improvement without game testing.

For each of at least three alternating ON/OFF paired runs:

1. Use the exact same game/save/route, resolution, clock profile and LSFG mode.
   Capture actual clocks, temperatures, and `build-info.txt` with each set.
2. Compare **actual independent game frames** (external observation), source
   present cadence, P95/P99 intervals, >50 ms and >100 ms outliers, draw
   counts per frame, and artifact correctness.
3. Compare CPU render ticks, texture sub-stages, descriptor bind outcomes, and
   update-call counts. Diagnostic CPU timings include profiling overhead.
4. Reject apparent FPS gains due to dropped draws, stale textures, incomplete
   CPU/GPU synchronization, visibly different scenes, or broken game speed.
5. If ON is not consistently better than OFF at equivalent workloads, remove
   or redesign the redundant-bind cache *without undoing the imageLayout
   descriptor invalidation correctness fix*.

Inspect two sessions individually:

```sh
node scripts/analyze-switch-performance.mjs logs/on --start-ms 100000 --end-ms 210000 > on.json
node scripts/analyze-switch-performance.mjs logs/off --start-ms 100000 --end-ms 210000 > off.json
```

The bounds are **not universal**: first locate the matching heavy workload
and select identical complete phases. Do not mix startup, loading, or exit
windows. Raw CSVs from the user's test sessions are not part of this repo.

## Acceptance boundaries

- CI compile and regression checks prove buildability and host invariants, not
  GPU correctness or an FPS gain.
- The default ON build must preserve behavior of the already merged base.
- Do not weaken texture invalidation or GPU synchronization to inflate FPS.
- A later texture/vertex optimization needs these sub-stage results plus a
  mechanism to invalidate reused data after guest/GPU writes.
