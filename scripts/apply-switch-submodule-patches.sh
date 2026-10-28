#!/usr/bin/env bash

set -euo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
PATCH_DIR="$ROOT/patches/submodules"

git -C "$ROOT" submodule update --init --recursive

apply_one() {
    local relative_repo="$1"
    local patch_name="$2"
    local expected_head="$3"
    local repo="$ROOT/$relative_repo"
    local patch="$PATCH_DIR/$patch_name"
    local actual_head

    actual_head="$(git -C "$repo" rev-parse HEAD)"
    if [[ "$actual_head" != "$expected_head" ]]; then
        printf 'error: %s is at %s; expected %s\n' \
            "$relative_repo" "$actual_head" "$expected_head" >&2
        exit 1
    fi

    if git -C "$repo" apply --reverse --check "$patch" >/dev/null 2>&1; then
        printf 'already applied: %s\n' "$relative_repo"
        return
    fi

    if ! git -C "$repo" diff --quiet || ! git -C "$repo" diff --cached --quiet; then
        printf 'error: %s has unrelated tracked changes\n' "$relative_repo" >&2
        exit 1
    fi

    git -C "$repo" apply --check "$patch"
    git -C "$repo" apply "$patch"
    printf 'applied: %s\n' "$relative_repo"
}

apply_one "external/concurrentqueue" \
    "concurrentqueue-switch.patch" \
    "9afb99746f0f5fc94ac8aef737053ae0481ba8d1"
# Later patches may change context used by an earlier patch's reverse check.
# Verify the complete stack newest-first in a private temporary index, without
# changing the real index or working tree. This also makes repeat CI builds
# independent of how the individual hunks overlap.
dynarmic_stack_applied() (
    local temp_index_dir
    temp_index_dir="$(mktemp -d)"
    trap 'rm -rf -- "$temp_index_dir"' EXIT
    export GIT_INDEX_FILE="$temp_index_dir/index"
    git -C "$ROOT/external/dynarmic" read-tree HEAD
    git -C "$ROOT/external/dynarmic" add -u -- .
    local patch_name
    for patch_name in dynarmic-switch-write-table.patch dynarmic-switch-write-probe.patch dynarmic-switch.patch; do
        git -C "$ROOT/external/dynarmic" apply --cached --reverse "$PATCH_DIR/$patch_name" >/dev/null 2>&1 || exit 1
    done
)

dynarmic_head="$(git -C "$ROOT/external/dynarmic" rev-parse HEAD)"
if [[ "$dynarmic_head" != "86458a0bd369d63ba4c2ef812cacbb6c9080c065" ]]; then
    printf 'error: external/dynarmic is at unexpected commit %s\n' "$dynarmic_head" >&2
    exit 1
fi
if dynarmic_stack_applied; then
    printf 'already applied: dynarmic Switch/write-probe/write-table stack\n'
else
    apply_one "external/dynarmic" \
        "dynarmic-switch.patch" \
        "86458a0bd369d63ba4c2ef812cacbb6c9080c065"
    # The original patch has already dirtied this submodule. Only apply the
    # additive layers, preserving the clean-tree check for the original patch.
    for patch_name in dynarmic-switch-write-probe.patch dynarmic-switch-write-table.patch; do
        patch_path="$PATCH_DIR/$patch_name"
        if git -C "$ROOT/external/dynarmic" apply --reverse --check "$patch_path" >/dev/null 2>&1; then
            printf 'already applied: %s\n' "$patch_name"
        else
            git -C "$ROOT/external/dynarmic" apply --check "$patch_path"
            git -C "$ROOT/external/dynarmic" apply "$patch_path"
            printf 'applied: %s\n' "$patch_name"
        fi
    done
fi
apply_one "external/ffmpeg" \
    "ffmpeg-switch.patch" \
    "02f4f2691b0efffff8923235baf146a87fc37263"
apply_one "external/LibAtrac9" \
    "libatrac9-switch.patch" \
    "82767fe38823c32536726ea798f392b0b49e66b9"
apply_one "external/sdl" \
    "sdl-switch.patch" \
    "fa2c02bb6e21974a89ea9824bc53c9932abe5f9c"
apply_one "external/psvpfstools/psvpfsparser" \
    "psvpfsparser-switch.patch" \
    "d14381f871a69009bd18b2aaec2213a6738bebba"

printf 'All Switch submodule patches are applied.\n'
