#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${ROOT}/build-switch-preflight/performance-tests"
mkdir -p "$BUILD_DIR"

for test in descriptor_binding_cache vertex_input sampler_mru; do
    "${CXX:-c++}" -std=c++20 -O2 -Wall -Wextra -Werror -pedantic \
        -I "$ROOT/vita3k/renderer/include" \
        -I "$ROOT/vita3k/util/include" -isystem "$ROOT/external/boost" \
        "$ROOT/vita3k/renderer/tests/$test.cpp" -o "$BUILD_DIR/$test"
    "$BUILD_DIR/$test"
done

python3 "$ROOT/scripts/tests/test_switch_cpu_checkpoint.py" --cxx "${CXX:-c++}"
python3 "$ROOT/scripts/tests/test_switch_tracelab_ram.py" --cxx "${CXX:-c++}"
python3 "$ROOT/scripts/tests/test_switch_hotspot_metric_layout.py"
python3 "$ROOT/scripts/tests/test_switch_render_core_spread.py"
