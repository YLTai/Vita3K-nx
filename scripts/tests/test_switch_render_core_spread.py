#!/usr/bin/env python3
"""Reject scheduling experiments that bypass the guest and helper priority rules."""

from pathlib import Path
import re

root = Path(__file__).resolve().parents[2]
impl = (root / "vita3k/util/src/switch_thread.cpp").read_text()
header = (root / "vita3k/util/include/util/switch_thread.h").read_text()
render = (root / "vita3k/renderer/src/batch.cpp").read_text()
cmake = (root / "vita3k/util/CMakeLists.txt").read_text()

start = impl.index("void switch_pin_render_thread(")
end = impl.index("\nvoid switch_allow_helper_core(", start)
body = impl[start:end]

assert "option(SWITCH_RENDER_CORE_SPREAD" in cmake
assert "VITA3K_SWITCH_RENDER_CORE_SPREAD=1" in cmake
assert "#ifdef VITA3K_SWITCH_RENDER_CORE_SPREAD" in body
assert "BACKGROUND_PRIORITY = 63;" in impl
assert "GUEST_PRIORITY_HIGHEST = 45;" in impl
assert "GUEST_PRIORITY_LOWEST = 56;" in impl
assert "ALL_CORE_MASK = 0b1111u;" in impl
assert "svcSetThreadPriority(CUR_THREAD_HANDLE, BACKGROUND_PRIORITY)" in body
assert "svcSetThreadCoreMask(CUR_THREAD_HANDLE, IDEAL_CORE_DONT_CARE, ALL_CORE_MASK)" in body
assert body.index("svcSetThreadPriority(") < body.index("svcSetThreadCoreMask("), (
    "lower thread priority before granting access to the app cores"
)
assert 'switch_pin_to_hardware_priority(what, BACKGROUND_PRIORITY);' in body
assert "switch_pin_render_thread(\"render thread\")" in render
assert 'switch_pin_to_hardware_priority("render thread", 63)' not in render
assert "void switch_pin_render_thread(const char *what);" in header
assert "inline void switch_pin_render_thread(const char *) {}" in header
# Ensure audio/display/vblank helpers retain their existing pinning and guest
# priority handling is untouched.
assert "switch_pin_to_helper_core" in impl and "switch_apply_guest_thread_priority" in impl
assert "switch_pin_to_hardware_priority" in impl
print("render spread build switch, priority order, safety fallback and desktop stub: OK")
