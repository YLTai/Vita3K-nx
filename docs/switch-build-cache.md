# Faster Switch edit/build cycles

The Switch CI workflow lives on `ci/switch-build` at
`.github/workflows/switch-build.yml`. It runs Ninja on a fresh GitHub Actions
container each time; without an object cache, even unchanged Vita3K sources
must be recompiled on every run.

## Local incremental development

Configure once, then **reuse the same build directory** (do not delete it
between edits). Keep your existing, pinned Switch SDK and OpenSSL locations.

```sh
cmake -S . -B build-switch-fast -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$DEVKITPRO/cmake/Switch.cmake" \
  -DCMAKE_BUILD_TYPE=Release \
  -DUSE_LTO=NEVER \
  -DCMAKE_C_COMPILER_LAUNCHER=ccache \
  -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
  -DSWITCH_OPENSSL_DIR="$SWITCH_OPENSSL_DIR" \
  -DSWITCH_VULKAN_SDK="$SWITCH_VULKAN_SDK"
cmake --build build-switch-fast --parallel 2
```

After changes to ordinary `.cpp` files, **only run the last build command**.
Ninja will compile changed inputs and relink; `ccache` also reuses prior
compilations when their source, headers, toolchain and flags match. The
`gitver.h` generator does not alter the header's timestamp when its content
is unchanged, avoiding unnecessary rebuilds after a CMake reconfigure.

Check the rebuild plan and cache effectiveness:

```sh
ninja -C build-switch-fast -n -d explain
ccache --show-stats
```

The resulting emulator core is `build-switch-fast/Vita3K.nro`. The launcher
is a **separate** Makefile build that embeds the core into its RomFS. This
local command builds the emulator core, not the all-in-one launcher artifact.

## GitHub Actions cross-run cache

The workflow now installs `ccache` and uses it for CMake C/C++ compilation
and the separate launcher Makefile. It restores a cache up to 2 GiB from the
most recent compatible prior run and saves an updated snapshot at the end,
including after normal build failures. This is a **compiler object cache**,
not a restored Ninja build tree or an incremental linker.

Cache groups include:

- The devkitPro package inventory digest.
- The SDK/OpenSSL preparation scripts and patches.
- The LTO mode (`RELEASE_ONLY` or `NEVER`).

A unique run ID/attempt suffix allows new entries to be saved even when
restoring a previous snapshot. `ccache` still verifies the compiler binary,
compilation options, source and relevant headers before reusing objects.

**Branch scope matters:** GitHub Actions can restore caches from the current
branch and the default branch; pull requests can also read their base branch's
cache. A cache saved by a PR run is scoped to that PR's merge ref and does
not automatically warm other PRs or the base branch. After merging this
change into `ci/switch-build`, the next **push build** of that branch creates
a shared warm cache for subsequent PRs targeting it.

### Choose fast builds versus performance builds

By default, CI keeps Release `-O3` **with LTO**, matching existing performance
artifacts. To turn off LTO for repeated CI development builds, create
the repository Actions variable `VITA3K_SWITCH_FAST_DEV` with value `true`:
Settings → Secrets and variables → Actions → Variables.

You may also select the `fast_dev` checkbox on a manual `workflow_dispatch`
run **once the workflow is available on the default branch**. While the
workflow exists only on `ci/switch-build`, its push/PR triggers work, but
GitHub does not expose the normal manual-run button.

Reset the repository variable to `false` or remove it before collecting
official performance comparisons. Each artifact's `build-info.txt` records
its `USE_LTO` mode.

### Checking whether the cache actually helps

Read the workflow logs:

- `Restore C/C++ object cache` shows the key that was restored.
- `Show compiler cache statistics` shows cacheable calls, hits and misses.

Because each save uses a new per-run key, `cache-hit` for the *primary* key
is not expected to be true; a restore-key prefix hit still restores useful
entries. The **first** run generally has to populate the cache. After it,
expect hits when the input content, compiler and flags are unchanged.

LTO's final link/LTRANS phase still runs in the default Release build and
cannot be skipped by `ccache`. Use `USE_LTO=NEVER` for quick local/CI
iterations, but compare final Switch FPS using matching Release/LTO builds.

The cache is best-effort: misses are safe (they just mean more compilation),
and GitHub's cache storage limits/evictions or toolchain updates may require
a new warm-up.
