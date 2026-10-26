# Switch NRO CI

Keep this workflow and `.ci/build-switch-sdk.sh` on `ci/switch-build`. Open the
code PR from `fix/lsfg-dialog`, which does not contain the CI additions. When
updating the fix, merge `fix/lsfg-dialog` into `ci/switch-build`, not the reverse.

After committing the CI files, push `ci/switch-build` to start **Build Switch
NRO**. No SDK URL, repository secret, or release publishing is required. GitHub
only shows the manual `workflow_dispatch` button when the workflow exists on
the default branch; the branch-specific push trigger works without that.
The desktop/Android workflow skips pushes to this CI branch.

On success, download the `vita3k-switch-<commit>` artifact:

- `Vita3K.nro`: the launcher with the emulator and its resources bundled inside.
- `core/Vita3K.nro`: the inner emulator, retained for diagnostics.
- `SHA256SUMS` and `build-info.txt`: checksums, source revision and toolchain versions.

## Build inputs

The upstream [building guide](https://github.com/NaGaa95/Vita3K-nx/blob/master/building.md)
covers desktop and Android builds. The Switch configuration comes from
[`cmake/switch.cmake`](../cmake/switch.cmake), the three `scripts/*switch*.sh`
helpers and [`switch-launcher/Makefile`](../switch-launcher/Makefile).

The workflow uses the devkitPro Linux container, applies the pinned submodule
patches, builds OpenSSL, builds the emulator with CMake/Ninja, then bundles it
with the launcher using Make. It uses the checked-in launcher version and assets.

Use the toolchain and Switch portlibs already installed in the official image.
Do not run `dkp-pacman -S`/`-Syu` in Actions: devkitPro
[blocks package downloads from GitHub CI](https://github.com/devkitPro/installer/issues/69).
The Mesa helper temporarily moves the packaged GL files aside and restores
them locally on exit; it does not uninstall or download any devkitPro packages.

The SDK helper reads the Mesa version and commit from
`scripts/prepare-switch-vulkan-sdk.sh` (currently 26.2.1 at
`efaf009f454e35fc6bcf4357e52455e30bfc3356`). No public download of the script's
exact SDK ZIP was found. Instead, the helper follows the
[Mesa Switch Linux CI recipe](https://github.com/danfromtico/mesa-switch/blob/main/.github/workflows/build.yml)
to build that commit from source. A CI-only patch adds `c99_alloca.h` to NVK's
physical-device source, which needs `alloca()` when `nvk-build-id` is set.
The SDK records the patch hash and actual output checksums, and
passes the repository's SDK `--check`. This does **not** reproduce or bypass the
unpublished ZIP's archive checksum; the ZIP download/extraction path is unused.

Mesa and OpenSSL are cached by the devkitPro package versions, scripts and patches.
The first run builds Mesa's host tools, Rust bindings and graphics libraries;
subsequent runs reuse the checked SDK. The container and Rust nightly follow
upstream's moving versions, so this is not a bit-for-bit reproducible build.
Failed runs upload available CMake/Meson configure logs for seven days.

Local syntax checks do not establish that an NRO compiles or runs on hardware.
The first Actions run must validate the complete cross-build; testing the NRO
on a Switch is a separate step.
