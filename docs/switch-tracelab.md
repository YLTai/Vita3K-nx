# Vita3K-nx TraceLab v2 — RAM-first diagnostic capture

The current validation branch defaults to the [single-NRO memory validation
suite](SWITCH_MEMORY_VALIDATION.md). It adds startup tests and an automatic
ABBA/BAAB staging experiment. While `memory_validation=auto` or `on` is active,
legacy CSV, per-store probes and unsafe Hash skips below are overridden to off.
Use the validation guide for this build; the historical opt-in experiments in
this document are not evidence of complete memory coherence.

This edition is intended for **slow TF/microSD cards**. It separates high
frequency measurements from storage completely: during an active game session,
the new TraceLab collects compact atomic counters, per-thread bounded event
queues and bounded RAM snapshots. It performs **no SD-card writes while the
game is running**. All output is written in batches when the game exits
normally, after guest/render workers have stopped.

The previous six-CSV profiler was a second source of SD-card stalls.
**It is now disabled by default in TraceLab builds**, including when
`mode=off`. It can be explicitly re-enabled if an old diagnostic comparison
requires its thread and stage CSVs.

## Config file

Create or edit `sdmc:/switch/vita3k/tracelab.cfg`, for example:

```ini
# Read at the beginning of every game session; no rebuild needed.
mode=auto
legacy_csv=off
```

| Setting | Purpose |
| --- | --- |
| `mode=off` | No TraceLab counters, timers or exports; suitable for a matched-build baseline |
| `mode=light` | Counter-only stage totals, render frames and sparse event trace; no sampled stage timers |
| `mode=cpu` | Counters plus sampled CPU/JIT/HLE phase wall time |
| `mode=render` | Counters plus sampled GXM/renderer/Vulkan stage wall time |
| `mode=auto` | **Default**: both groups; random ~1/32 stopwatch samples |
| `mode=deep` | Both groups; random ~1/8 stopwatch samples plus render-begin and per-batch events |
| `legacy_csv=on` | Also run old profiler: six older files, frequent synchronous writes and more overhead |

The configuration is independent of the launcher's regenerated `config.yml`.
Start with `auto` and `legacy_csv=off`. Do **not** use `deep` for FPS
comparison; it has more overhead. For recording old per-core CPU tick and
fine-grain render phase CSVs, run a separate session with `legacy_csv=on`.
Never compare different profiler modes as if they were identical hardware loads.

## What is recorded

TraceLab measures these categories: Dynarmic JIT Run and code-cache recycling;
sampled HLE calls grouped by NID; Guest scheduling acquisition/yield; GXM
front-end Draw and uniform/state preparation; command pool and batch handling;
texture lookups, hashing, hash changed/unchanged/scene-reused outcomes, uploads;
index scans and vertex binds; External Host cache clean/flush byte ranges;
descriptor set resolve/update/bind; pipeline lookup and worker compilation;
Vulkan queue submit/present; render loop timing; and the collector's own CPU
side polling cost.

New counters for texture hash outcomes are **observational only**: the cache
invalidation, hashing, GPU sync and upload behavior remain unchanged.

`threads.csv` additionally captures available Horizon **total CPU tick**
deltas from in-thread checkpoints, rather than relying on the known-unreliable
per-core percentages. A label identifies guest, render, frontend, or shader
worker when available. CPU times may miss up to the final checkpoint period
and cannot independently separate HLE waiting from actual computation.

### A complete session directory

After *normal exit*, look under
`sdmc:/switch/vita3k/logs/tracelab-<TITLE>-<epoch>-<run>/`:

```text
manifest.json      schema=2, modes, timebase, frame counts, overflow flags
hotspots.csv       ~1 Hz counter and random-sampled stopwatch snapshots
frames.csv         independently recorded render-loop completion timestamps
nid-samples.csv    sampled HLE NIDs aggregated in RAM by producer thread
threads.csv        self CPU total tick checkpoints for each producer
trace.bin          sparse binary events, sorted by CPU tick on normal exit
summary.json       full-session metric totals and sample tick sums
```

**No files are written by TraceLab while the game is running.** File creation,
CSV formatting and `fwrite` happen during normal exit and may be slow on a
slow TF card. This trade-off preserves gameplay measurement fidelity.
**A crash or forced HOME exit may lose the entire in-RAM TraceLab session.**
Legacy CSVs, if enabled, have their old write/flush behavior.

### Bounded RAM usage and data integrity

TraceLab bounds its allocation sizes (per-session, not continually growing):

- Up to 64 registered producer slots.
- Up to 1024 pending events per slot; up to 65,536 preserved trace events
  (~1.5 MiB of binary trace payload).
- Up to 32,768 independently recorded render-loop completions
  (~512 KiB in RAM, about 22 minutes at 25/s).
- Up to 32,768 metric-window rows (compact in-memory records).
- Up to 128 distinct sampled NIDs per producer; rare overflow is explicitly counted.

`manifest.json` records `frames_overflow`, `events_dropped`,
`window_rows_overflow` and `nid_samples_overflow`. If nonzero, report the
lost coverage rather than inventing a complete percentile or hotspot ranking.

The RenderEnd timestamp now goes into its **own fixed frame array**, not
through the event queue. Missing non-frame trace events therefore do not
silently produce huge fake frame intervals. Only frames beyond the declared
frame array limit are omitted from `frames.csv`.

### Interpreting measurements

- `hotspots.csv` retains the v1 schema for compatibility; `thread_slot=0`
  means the new RAM aggregator has **merged all producers** for that metric
  in that time window. Exact per-thread CPU checkpoints are in `threads.csv`.
- `estimated_wall_ms` of a 1-second row is meaningful only if
  `sampled_calls > 0`. The PC analyzer now **pools samples across all selected
  windows before extrapolating**, instead of summing zero-sample windows as zero.
- Timings are CPU-side **wall time** and may include blocking, preemption or
  driver waits. GXM, texture, and draw nested spans overlap; do not add them.
- NID summary data is **full-session only**. A report with
  `--start-ms/--end-ms` filters frame and hotspot windows but must still
  label the HLE NID table as full-session.
- Trace events contain GXM NewFrame commands, batch consumption, render-end,
  long-frame markers, shader pipeline events and JIT cache recycling.
  High-frequency HLE events are **not** emitted by default.
- `render_loop` is not simulated PSV FPS, independent unique game frames,
  or physical panel scanout. GPU execution timestamp is **unmeasured**.
- The collector's `collector_poll` metric now covers RAM aggregation only;
  output writes occur after measurement ends.

## PC analysis

Requires Node.js but no npm dependencies:

```sh
node scripts/analyze-switch-tracelab.mjs ./tracelab-PCSD00035-... \
  --start-ms 100000 --end-ms 210000 \
  --html report.html --trace-json events.json > report.json
```

The analyzer supports **old schema v1** traces as well, using contiguous
frame IDs to filter missing-event gaps. For v2 it reports pooled hotspot
estimates, independent frame interval statistics, aggregated NID samples
(full session), and thread CPU checkpoint data.


## Direct optimization trial: JIT-CPU guarded texture reuse (UNSAFE)

The PCSD00035 A/B/C logs from 2026-10-28 already answered the cost
question. In the 100–170s heavy window, full-Hash render loops/second
were 24.36 (JIT instrumentation OFF), 23.36 (count every Guest store),
and 22.06 (count + texture-page lookup). Of ~2.835 **billion** direct JIT
stores in C, only **611** landed on pages *ever* used as texture
input during that heavy window (~0.000022%). Yet startup/menu periods
showed millions of such writes.

Rather than repeat three counter experiments, this is a **single new
opt-in direct performance test**. Use the **same NRO** and the following
config in `sdmc:/switch/vita3k/tracelab.cfg`:

```ini
mode=auto
legacy_csv=off
texture_hash_probe_interval=0
unsafe_texture_hash_skip_interval=0
unsafe_texture_hash_recheck_interval=64
guest_write_probe=guard
```

`guard` differs from the old `texture` counting mode:

- The JIT does **not** increment the global Guest-write counter.
  It performs a lightweight page-membership lookup after each
  successful direct Guest store; a store to a registered texture page
  sets a **sticky one-byte taint** for that page using AArch64
  release-store semantics. Guest fallback and exclusive writes set
  the same taint through their existing callbacks.
- Texture pages are registered only on cache misses (one byte per
  4 KiB Guest page, 1 MiB table). The taint table is another 1 MiB.
  Both tables are bounded RAM storage and reset on title startup.
- A texture whose entire source range has remained **untainted by
  observed JIT CPU writes** may still use the existing unsafe
  every-64-uses Hash throttle. If **any** of its pages has a
  sticky taint, every subsequent use performs the original full
  Hash until the end of the title. No attempt is made to reset
  the taint after that Hash. Palette P4/P8 textures always use full
  Hash in guard mode, because the palette lives outside the watched
  texture pixel range.
- `texture_hash_guard_blocked` counts the opportunities in which
  a tainted page forced a full Hash instead of a candidate skip.
  `guest_write_guard_tainted_pages` in the manifest is the number
  of unique tainted Guest pages at normal exit.
- The original `off`/ `count`/ `texture` modes are preserved:
  `off` is the original JIT path, and `count` / `texture`
  still **force both unsafe Hash skipping knobs off**.

The expected opportunity is to avoid the old `texture` mode's
~9% heavy-scene slow-down from per-store counters while maintaining
most of the ~13% speed-up measured with `64` alone. **This is a
hypothesis, not a measured net speed-up.** There is no need to rerun
the A/B/C counter experiment: make only **one new guard-mode run**,
using the same warmed PCSD00035 save and scene, preferably logging
100–170s and exiting normally. Compare this guard run with the
existing A baseline (full Hash) and older 64-use trial (unsafe skip).
Check both performance and visual behavior.

**IMPORTANT — deliberately NOT lossless.** The JIT guard notices only
successfully executed Guest CPU writes that it instruments, and only
the starting page of fast-path stores. It does **not** trace all HLE
pointer writes, GPU shader and surface writes, aliases, or palette-only
inputs. A concurrent write/renderer race can also evade sampling without
a complete synchronization protocol. The old 64-use post-fact audits
still apply; a clean audit **cannot prove** that a skipped texture was
unchanged. A black/flickering menu or any audit mismatch means this
experimental mode is unsuitable for normal use. Keep these new
behaviors OFF by default.

To restore the original complete Hash behavior with the same NRO,
set `guest_write_probe=off` and
`unsafe_texture_hash_recheck_interval=0`.

---

## Dynarmic ARM64 Guest write-count experiment (safe Hash, 1 NRO)

The `PCSD00035` heavy scene previously spent ~9.7 ms/render loop
on full texture Hash. The intentionally unsafe 64-use skip trial
improved throughput by ~13%, but revealed content mismatches.
This new experiment measures **whether a software write barrier would eat
that potential gain** without relying on stale texture data.

**Important:** All three runs use the SAME NRO with FULL Hash.
The `count` and `texture` write-count experiments
automatically set both `unsafe_texture_hash_recheck_interval` and
`unsafe_texture_hash_skip_interval` to zero, even if an old `64`
value is left in the config. The explicit `guard` mode described
above is the sole exception: it intentionally allows unsafe counted
Hash skipping, with additional CPU-JIT taint disqualification. Keep both at zero
for the baseline as well.

Save this common config to `sdmc:/switch/vita3k/tracelab.cfg`:

```ini
mode=auto
legacy_csv=off
texture_hash_probe_interval=0
unsafe_texture_hash_skip_interval=0
unsafe_texture_hash_recheck_interval=0
guest_write_probe=off
```

Change **only the last value**, restart the same game, and replay the
same heavy scene and duration:

| Mode | `guest_write_probe` | Dynarmic JIT work |
| --- | --- | --- |
| A | `off` | Original fast page-table STR instructions, **no extra JIT instructions** |
| B | `count` | Count successful direct-JIT stores with a per-JIT non-atomic u64 counter |
| C | `texture` | Same total counter **plus** one bitmap lookup and a separate counter if the starting 4 KiB page has ever been used as a texture in this session |

The 4 GiB Guest address space uses a **1 MiB monotonic byte-per-page map**
(1,048,576 × 4 KiB), allocated only in C mode. TextureCache
registers pages **only on cache misses**, avoiding an O(texture_size)
walk on every draw. A page remains registered even after the texture
is evicted. The C-mode count is therefore an approximate *upper bound*
on CPU writes to currently relevant texture pages, not a live dirty
tracker or evidence that the texture contents changed.

The new instrumentation is in
`patches/submodules/dynarmic-switch-write-probe.patch`.
It only modifies the ARM64 page-table write fast path on Switch and
emits the extra instructions at JIT compilation **only when enabled**.
Callback-only and exclusive Guest writes are counted separately by
Vita3K's existing slow-path memory callbacks. Fast-path counts use
plain per-JIT counters, flushed to per-thread RAM aggregates only when
`jit->Run()`/`jit->Step()` returns. No SD writes occur during gameplay.

The following metrics are available in `hotspots.csv`, `summary.json`,
and the PC `guest_write_probe` analysis object:

| Metric | What it counts |
| --- | --- |
| `guest_jit_fast_writes` | Successful native page-table Guest STR operations |
| `guest_jit_texture_page_writes` | Fast writes whose **starting page** is in the ever-texture-page bitmap |
| `guest_jit_slow_writes` | Successful callback/fallback/exclusive Guest writes |
| `guest_jit_slow_texture_page_writes` | Callback writes to a registered texture page |
| `guest_texture_pages_registered` | Unique pages ever marked by a texture cache miss |

The manifest records `guest_write_probe_mode` (0/1/2/3)
and `guest_write_probe_texture_pages` (number of distinct registered pages).
Since mode A deliberately does not instrument Guest stores, its
write-count values are zero **by design**, not evidence the CPU did not
write memory.

### Interpretation and limitations

Compare 100–170 seconds of the *same* `PCSD00035` heavy scene with
unchanged clocks, save state, rendering settings, LSFG and shader cache.
Measure A → B to estimate the cost of **counting all Guest writes** and
B → C to estimate the extra cost of **checking texture page relevance**.
Use frame-time P50/P95/P99, render loops per second, JIT code cache
recycles, and the write counts per render loop. Do not treat these as
real display FPS or a controlled CPU microbenchmark.

**Do not use these counters to skip Hash**: HLE pointer writes, GPU
shader/surface writes and aliased-memory invalidations are not tracked
by this experiment. A 64/128-bit unaligned store may cross a page
boundary; only its first page is classified. In addition, JIT
callback slow-path stores may be attempted on unmapped ranges; the
slow count records only successfully completed stores. Out-of-range
guest descriptors are ignored when registering texture pages.

If C is expensive, a targeted/once-dirty JIT barrier is still possible,
but requires a *different* design and complete HLE/GPU coherence before
it can replace full Hash. The current experiment intentionally makes
**zero cache correctness decisions**.

---

## Aggressive cross-scene hash reuse A/B (UNSAFE; intended for heavy scenes)

The measured PCSD00035 heavy scene has roughly **nine scene boundaries per
render loop**, so the original `unsafe_texture_hash_skip_interval=8`
scene-epoch policy skips effectively nothing. The new **reuse-count policy**
deliberately ignores the size of the scene gap. It verifies one full hash per
N cached cross-scene texture *uses*, rather than at each Nth scene epoch.

This is an opt-in **graphics-correctness experiment**. There is still no
complete way to detect every CPU/HLE/GPU guest-memory write, and a skipped
hash can display stale textures for several frames. Do not enable this on a
build used for ordinary gameplay or assume that a clean audit proves safety.

**A (original full hashing, same instrumented NRO):**

```ini
mode=auto
legacy_csv=off
texture_hash_probe_interval=0
unsafe_texture_hash_skip_interval=0
unsafe_texture_hash_recheck_interval=0
```

**B (aggressive: after a confirmed unchanged recheck, skip 7 of every 8
cached cross-scene uses):**

```ini
mode=auto
legacy_csv=off
texture_hash_probe_interval=0
unsafe_texture_hash_skip_interval=0
unsafe_texture_hash_recheck_interval=8
```

Permitted nonzero recheck intervals are `2`, `4`, `8`, `16`, `32`,
and `64`; `0` is the safe default. Larger values are more aggressive.
If both the old scene-epoch and new recheck-count knobs are nonzero, the
**reuse-count mode takes precedence** and turns off the old mode.
`mode=off` disables both experiments.

The throttle is specific to one cached texture entry:

- First cache insertion performs a complete hash; the next complete
  verification must also be unchanged before the policy starts skipping.
- Under interval 8, up to seven subsequent distinct-scene cached uses are
  skipped. Use number eight performs a complete audit regardless of whether
  the scene epoch advanced by 1, 9, or 200.
- An entry that has ever changed during its lifetime is excluded from
  further skipping until its cache slot is recycled.
- A change detected after skipped uses **quarantines that texture**, but
  unlike the old cautious mode it **does not abort the entire experiment**.
  Other eligible textures continue to skip, so the heavy-scene test remains
  meaningful.
- If a skipped texture is evicted or its original mapping cannot be
  audited, mark the trial as containing **unverifiable skipped data**.
  The removed entry cannot reuse its former state. Other entries continue.
- Imports/exports, invalid ranges and same-scene memoization retain their
  existing handling. No modification of CPU DCache Clean/Flush, GPU barriers
  or resource uploads is intended in the full-hash A mode.

New summary counters:

| Metric | Description |
| --- | --- |
| `texture_hash_aggressive_skipped` | Full hashes skipped; byte total is the estimated source range not rescanned |
| `texture_hash_aggressive_audits` | Full rechecks of textures after one or more skipped uses |
| `texture_hash_aggressive_quarantines` | Confirmed content changes after skipped uses; earlier frames **may be wrong** |
| `texture_hash_aggressive_unverified` | LRU eviction or invalidation prevented confirmation of skipped content |

The manifest also records
`unsafe_texture_hash_recheck_requested_interval`,
`unsafe_texture_hash_recheck_final_interval`,
`unsafe_texture_hash_recheck_mismatch_seen` and
`unsafe_texture_hash_recheck_unverified_seen`.
The PC analyzer adds a `texture_hash_aggressive` object and a clearly
labeled warning section in HTML.

Run both A and B for the same warmed `PCSD00035` heavy scene, ideally
90–120 seconds each, with matching clocks, image resolution, save,
LSFG state and rendering config. Use `--start-ms 100000 --end-ms 170000`
for the same analysis interval if the recordings contain that route.
Compare rendered-loop throughput, actual contiguous frame-time P50/P95/P99,
hash bytes and wall-time samples, and visual correctness. **Do not infer
from the expected 7/8 Hash skip ratio that FPS must improve 7/8 as much**:
the 9–10 ms/loop full-hash wall-cost estimate may overlap CPU/GPU work or
include scheduling.

If this test has a clear speedup but visual errors or quarantines,
the underlying opportunity is real but the algorithm is **not release
safe**. The next correct step is an authoritative write-version system
covering Guest JIT stores, HLE writes, GPU shader stores and aliased
memory—not a blind increase to interval 64.

---

## Heavy-scene texture Hash eligibility diagnosis (safe, default)

The initial A/B trial of PCSD00035 skipped hashes only in the lighter early
scenes, **zero in the sustained heavy scene**. Rather than increasing an
unsafe interval, TraceLab now records *why* a cached texture does or does not
qualify for the existing throttle state machine.

Run the exact-hash baseline with the following configuration:

```ini
mode=auto
legacy_csv=off
texture_hash_probe_interval=8
unsafe_texture_hash_skip_interval=0
```

**This probe does not skip hashes, bypass cache maintenance, alter scene
numbers or change upload decisions.** The unsafe experiment remains fully
disabled. The independent probe interval defaults to `8` (allowed `0`,
`4`, `8`, `16`, `32`), so existing `auto` configurations get the new
reason counters without extra rebuilds or config edits. Setting the probe
to `0` suppresses its reason counters only, **not** the full Hash.

Each eligible cached, cross-scene lookup is placed in one of the following
mutually exclusive reasons. Counters include call counts and the inspected
texture source range in bytes; they are not physical DRAM traffic:

| Counter | Meaning |
| --- | --- |
| `texture_hash_probe_eligible` | Would qualify under *current verification history* and an intact mapping |
| `texture_hash_probe_warmup` | Fewer than 8 independent unchanged scene verifications |
| `texture_hash_probe_changed` | This cache entry was previously observed to change |
| `texture_hash_probe_audit_due` | Epoch gap since last full verification is at least the interval |
| `texture_hash_probe_nonmonotonic` | Epoch did not advance relative to last verification |
| `texture_hash_probe_no_epoch` / `texture_hash_probe_no_baseline` | Missing usable scene timestamp or baseline |
| `texture_hash_probe_import_export` | Replacement, import or export path cannot use the throttle |
| `texture_hash_probe_no_range` / `texture_hash_probe_invalid_mapping` | Source range cannot safely be used for the probe |
| `texture_hash_probe_new_entry` | Cache misses requiring initial full Hash; useful for diagnosing LRU churn |

An independent epoch-gap histogram reports
`texture_hash_epoch_delta_1`, `_2_7`, `_8_15`, `_16_31`,
`_32plus`. Large `_8_15` and `_32plus` bins point toward textures
being accessed many scene boundaries apart, rather than low Hash cache
hit rates.

### Per-Context scene epoch diagnostics

The shared Switch texture cache receives local `scene_timestamp` values
from different Vulkan contexts. The following events are counted at
**every scene assignment**:

- `texture_scene_boundary`: assignment occurred.
- `texture_scene_context_switch`: a different context assigned the next
  epoch value.
- `texture_scene_epoch_rewind`: assigned local epoch decreased relative to
  the previously assigned value.
- `texture_scene_epoch_collision`: context changed, but the assigned local
  epoch *equals* the previous context's epoch.

**Do not interpret these as proof that cache coherence is broken.** They
measure whether the current per-context epoch model is consistent enough
for a cross-context throttle. This revision intentionally **does not
replace** the existing `memo_scene` with a new global epoch; that change
could affect same-scene Hash memoization and unrelated buffer sync rules.
We will decide only after real-world counters and a dedicated correctness
test.

`node scripts/analyze-switch-tracelab.mjs <session-dir>
--start-ms 100000 --end-ms 170000` writes
`texture_hash_probe` to the JSON report, including an exclusive rejection
breakdown, byte ranges, epoch-gap histogram, context switches, rewinds and
collisions. Its HTML output includes the most frequent reasons.

**Important:** `eligible` is a counterfactual count in the baseline,
not a measurement of guaranteed savings. In A, full hashing updates
`last_verified_scene` on every use; in B, skipped checks do not.
The two distributions are not automatically identical.

## Experimental cross-scene texture hash A/B (intentionally UNSAFE)

The heavy `PCSD00035` scene produced almost entirely unchanged re-hash results,
yet there is **no complete CPU/HLE/GPU guest-memory write tracking on Horizon**.
A cross-scene skipped hash can therefore display **stale texture data**, even if
a later audit finds the change. This is a temporary experimental diagnostic
facility, **not a safe optimization** and **not a production default**.

Use two **separate** game launches from the same warmed shader-cache/save/route,
with comparable clocks, resolution, temperature, renderer and LSFG settings.
Leave the RAM-first profiler enabled in the *same* mode in both tests:

**A (correctness baseline, always complete hash)**

```ini
mode=auto
legacy_csv=off
texture_hash_probe_interval=8
unsafe_texture_hash_skip_interval=0
```

**B (explicit risky trial, audits every 8 scene epochs)**

```ini
mode=auto
legacy_csv=off
texture_hash_probe_interval=8
unsafe_texture_hash_skip_interval=8
```

The only accepted nonzero intervals are `4`, `8`, `16`, and `32`.
Default and invalid values are `0`. With `mode=off`, unsafe skipping is
automatically disabled: a hidden, unmonitored experiment is not allowed.

Implementation properties:

- New textures and recycled LRU slots start with a **full hash**.
- A cached texture needs **eight separate unchanged cross-scene full
  verifications** before any hash can be skipped.
- A texture that has ever changed since its cache entry was created is never
  selected for skipping again until its cache slot is recycled.
- Import/export/replacement texture paths, invalid texture ranges, zero-size
  textures, scene epoch zero and non-monotonic scenes always use the original
  full-hash path. A backwards scene epoch resets the stable warm-up counter.
  The existing same-scene memo remains intact.
- The first full verification after one or more skips is an **audit**. If
  changed bytes are detected (or a previously skipped range is now invalid),
  all unsafe hash skipping is disabled **for the rest of that game session**.
  The changed texture follows the normal upload/update path.
- An LRU eviction, import/export transition, or unmapped source while a
  skipped hash remains unaudited also disables the experiment for that session.
  This is recorded separately as an incomplete audit, not as a detected
  content mismatch.
- This is **detection after the fact**, not prevention: an update that occurs
  immediately after a skipped verification could produce stale pixels for
  several scene epochs, and a transient write-and-restore between audits may
  remain completely undetected. A zero mismatch count is **not proof of
  correctness**.

Additional TraceLab counters:

| Counter | Meaning |
| --- | --- |
| `texture_hash_unsafe_skipped` | Full-hash operations omitted; `bytes` is the estimated source range not rescanned |
| `texture_hash_unsafe_audit` | Subsequent full verifications following skips |
| `texture_hash_unsafe_audit_mismatch` | Full-hash content change observed after prior skips; unsafe mode disabled |
| `texture_hash_unsafe_unverified` | Skipped ranges lost auditability through eviction/mapping/replacement transition; unsafe mode disabled |
| `texture_hash_changed` / `texture_hash_unchanged` | Original **actually executed** full verifications, not skipped checks |
| `texture_hash_scene_reused` | Existing within-scene memoizations, independent of the unsafe experiment |

`manifest.json` records `unsafe_texture_hash_skip_requested_interval`,
`unsafe_texture_hash_skip_final_interval`, and
`unsafe_texture_hash_audit_mismatch_detected` and
`unsafe_texture_hash_audit_incomplete`. The PC analysis script adds
`texture_hash_experiment` to JSON/HTML. If `audit_mismatch_detected=true`,
discard that B run as a correctness failure and prefer `A`. Also discard
an experiment where `audit_incomplete=true`: unchanged audit counts do not
prove its earlier skips were safe.

For the initial A/B, test the same sustained **90–120 seconds** of demanding
gameplay, ensure there are no visible missing textures/artifacts, and compare
`frames.csv` frame-ID continuity, source-loop throughput and P95/P99
intervals. Compare CPU-side sampled `texture_hash` cost and scan bytes, plus
`dcache_clean` (not modified by this experiment). Do **not** sum nested
wall-clock phases or mistake estimated saved hash bytes for actual DRAM traffic.

Even if the experiment increases FPS, a release-worthy permanent change still
requires an authoritative version/dirty-tracking mechanism for every possible
guest CPU, HLE, GPU and aliased-memory writer.

## First validation run

1. Build/deploy the updated debug NRO, using the same Vulkan/NVK backend.
2. Start with `mode=auto`, `legacy_csv=off` and LSFG disabled.
3. Warm the shader cache; measure 90–120 seconds in a repeatable heavy scene.
4. Exit the game normally and allow the TF card to finish saving.
5. Verify `frames_overflow=0`, `events_dropped=0` or near zero,
   `window_rows_overflow=0`, and that `frames.csv` IDs are continuous.
6. Check that `collector_poll` elapsed time is significantly lower than
   the earlier 175 ms sampled average from the SD-writing collector.
7. Re-run using `mode=off` (also with `legacy_csv=off`) to estimate the
   remaining TraceLab runtime impact on source-frame cadence.

Once the diagnostic fidelity is validated, examine `texture_hash_unchanged`,
`texture_hash_changed` and `texture_hash_scene_reused` alongside
`texture_hash` and `dcache_clean` bytes/time. A high unchanged ratio can
motivate a **separate** cache-version tracking experiment, but it does not
by itself make it safe to remove memory coherence operations.

This change does not include experimental GPU timestamps, asynchronous
network streaming, unsafe cache-skipping optimizations or new scheduler
affinity rules.
