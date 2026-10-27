# Render core-3 preference without pinning

A recent real-Switch observation is that core 3 stays under ~60% while
application cores 0–2 approach 90%. The previous cross-core render migration
improved game performance, so **do not restore exclusive core-3 pinning**.

## Change

`switch_pin_render_thread()` previously requested:

```cpp
svcSetThreadCoreMask(CUR_THREAD_HANDLE, -1, 0b1111);
```

With `SWITCH_RENDER_PREFER_CORE3=ON`, it now requests:

```cpp
svcSetThreadCoreMask(CUR_THREAD_HANDLE, 3, 0b1111);
```

The **allowed-core mask remains 0b1111**: the thread is still eligible to
run on cores 0–3. Only the **preferred/ideal core** changes to 3. This is a
scheduler preference, *not a reservation* or a guarantee that a preferred
thread will be migrated whenever core load changes. If Horizon rejects that
request, the code retries `-1, 0b1111` (the known working unbiased policy),
then the original restricted fallback.

Render priority stays at **63** and is set **before** affinity. Guest
threads retain priorities 45–56 on cores 0–2. VBlank, audio, display and
Vulkan helper affinity and priorities stay unchanged and outrank rendering.
Nothing changes GPU fences, commands, caches, draw calls or clocks.

## Build and A/B

This PR's Switch CI produces two artifacts built from the same source
and pinned SDK, with identical render priority and allowed-core mask:

- `vita3k-switch-<CI-SHA>`: **preferred core 3** (ON).
- `vita3k-switch-render-unbiased-<CI-SHA>`: original **no preferred core**
  policy (OFF).

Both bundles contain `build-info.txt`, launcher/core NROs and SHA256SUMS.
Only the PR branch builds the extra OFF artifact. Other branches build one.

Compare the same PCSD00035 game route, clocks, resolution and warmed cache
with LSFG off, preferably in alternating ON/OFF runs. Acceptance requires
higher or equal visible game FPS and a non-regressing P95/P99 frame cadence,
audio and input behavior. Check the render thread's `self_core` reports
to verify the preference has an observable effect, and make sure guest work
isn't delayed. Historical per-core kernel-tick CSV fields are unreliable;
an external overlay may corroborate but is not proof of per-thread time.

If the preferred-core request is rejected, or repeated A/B tests show
performance or frame-time regressions, build with
`-DSWITCH_RENDER_PREFER_CORE3=OFF` or revert this change.

**No performance gain has been measured from this new change yet.**
