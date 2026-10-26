#!/usr/bin/env bash
# Check the Rust dependency closure using the same NVK prelink as Vita3K.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SDK="${SWITCH_VULKAN_SDK:?Set SWITCH_VULKAN_SDK to the packaged Mesa SDK}"
export DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
CHECK_DIR="$ROOT/build-switch-preflight/mesa-link"
mkdir -p "$CHECK_DIR"

bash "$ROOT/vita3k/switch/localize_mesa_vulkan.sh" "$SDK" \
    "$CHECK_DIR/libvulkan_local.o" "$CHECK_DIR/libvulkan_local.a"

# All Rust crates, including std, must be supplied by libvulkan.a. Leave the
# C/C++/libnx references for the final link, but reject missing Rust symbols
# now, before caching the SDK or compiling the entire emulator. Check both
# v0 and legacy Rust mangling, plus the unmangled runtime entrypoints.
"$DEVKITPRO/devkitA64/bin/aarch64-none-elf-nm" --undefined-only --format=posix \
    "$CHECK_DIR/libvulkan_local.o" > "$CHECK_DIR/undefined-symbols.txt"
if ! awk '
    $1 ~ /^(_R|_ZN[0-9]+(alloc|core|std)[0-9]|__rust|rust_eh_personality$)/ {
        print
        failed = 1
    }
    END { exit failed ? 1 : 0 }
' "$CHECK_DIR/undefined-symbols.txt"; then
    echo 'Mesa SDK prelink has unresolved Rust symbols; check its archive index and bundled Rust libraries.' >&2
    exit 1
fi

echo 'Mesa SDK prelink resolved all Rust runtime symbols.'
