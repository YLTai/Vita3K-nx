#!/usr/bin/env python3
"""Preflight: verify guest-write profiling is opt-in, wired, and Hash-safe.

Runs after scripts/apply-switch-submodule-patches.sh in GitHub Actions.
No game, graphics SDK or Switch toolchain is required.
"""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]
dyn = ROOT / "external/dynarmic/src/dynarmic"
p = ROOT / "patches/submodules/dynarmic-switch-write-probe.patch"
main = (dyn / "backend/arm64/emit_arm64_memory.cpp").read_text()
a32_cfg = (dyn / "interface/A32/config.h").read_text()
arm_cfg = (dyn / "backend/arm64/emit_arm64.h").read_text()
a32 = (dyn / "backend/arm64/a32_address_space.cpp").read_text()
cpu = (ROOT / "vita3k/cpu/src/dynarmic_cpu.cpp").read_text()
cache = (ROOT / "vita3k/renderer/src/texture/cache.cpp").read_text()
trace = (ROOT / "vita3k/util/src/switch_tracelab.cpp").read_text()
trace_h = (ROOT / "vita3k/util/include/util/switch_tracelab.h").read_text()
doc = (ROOT / "docs/switch-tracelab.md").read_text()
patch = p.read_text()

for key in ("switch_write_probe_total", "switch_write_probe_texture",
            "switch_write_probe_pages", "switch_write_probe_taint"):
    assert key in a32_cfg, key + " missing UserConfig"
    assert key in arm_cfg, key + " missing EmitConfig"
    assert key in a32, key + " missing A32 config forwarding"
    assert key in main, key + " missing JIT generation"
    assert key in patch, key + " missing patch"

# Exactly one instrumentation block: after the store succeeds, before the
# deferred fallback block. Fallback writes use the old MemoryWrite callback.
assert main.count("if (ctx.conf.switch_write_probe_total) {") == 1
store_pos = main.index("EmitMemoryStr<bitsize>(code, Rvalue->index(), Xbase, Xoffset, ordered);")
probe_pos = main.index("if (ctx.conf.switch_write_probe_total) {")
fallback_pos = main.index("ctx.deferred_emits.emplace_back", store_pos)
assert store_pos < probe_pos < fallback_pos
assert "code.UBFX(Xscratch1, Xaddr, 12, 20)" in main
assert "code.CBZ(Wscratch0, untracked)" in main
assert "code.STLRB(Wscratch1, Xscratch0)" in main
assert "ctx.conf.switch_write_probe_taint" in main
assert "if (ctx.conf.switch_write_probe_pages &&" in main
assert "code.l(untracked)" in main
assert "mcl::bit_cast<u64>(ctx.conf.switch_write_probe_total)" in main

# Uninstrumented runs never set the JIT counter pointer and can use the
# exact original guest page-table STR path. Counted runs don't skip Hash.
assert "if (config.page_table) {" in cpu
assert "if (probe_mode == 1 || probe_mode == 2)" in cpu
assert "else if (probe_mode == 3)" in cpu
assert "config.switch_write_probe_total = &write_probe_counters_.fast;" in cpu
assert "flush_write_probe();" in cpu
assert "tracelab::count_many(tracelab::Metric::GuestJitSlowWrites" not in cpu
assert "tracelab::guest_write_probe_mark_texture(" in cache
assert "tracelab::guest_write_guard_range_tainted(" in cache
assert "TextureHashGuardBlocked" in cache
assert "tracelab::guest_write_guard_note_callback_write(" in cpu
assert "config.switch_write_probe_taint =" in cpu
assert trace.count('if (value == "count") opt.guest_write_probe = 1;') == 1
assert trace.count('else if (value == "texture") opt.guest_write_probe = 2;') == 1
assert trace.count('else if (value == "guard") opt.guest_write_probe = 3;') == 1
assert "guest_write_guard_tainted_pages" in trace
assert "guest_write_guard_range_tainted(" in trace
assert "const bool requested_guard_failed =" in trace
assert "options.guest_write_probe == 3 && write_probe != 3" in trace
assert "|| requested_guard_failed ? 0 : options.unsafe_recheck_interval" in trace
assert "|| options.guest_write_probe != 0 || aggressive_interval != 0" in trace
assert "uint64_t* switch_write_probe_total" not in cpu

# The counter names must remain in sync with the exported layout.
enum_text = re.search(r"enum class Metric\s*:\s*uint8_t\s*\{(.*?)\};", trace_h, re.S).group(1)
enum_names = [x.strip() for x in re.sub(r"//[^\n]*", "", enum_text).split(",") if x.strip()]
assert enum_names[-1] == "Count"
labels = re.findall(r'"([^"]+)"', re.search(
    r"constexpr std::array<const char \*, kMetrics> kNames = \{(.*?)\};", trace, re.S).group(1))
assert len(enum_names) - 1 == len(labels)
for name, label in {
    "GuestJitFastWrites": "guest_jit_fast_writes",
    "GuestJitTexturePageWrites": "guest_jit_texture_page_writes",
    "GuestJitSlowWrites": "guest_jit_slow_writes",
    "GuestJitSlowTexturePageWrites": "guest_jit_slow_texture_page_writes",
    "GuestTexturePagesRegistered": "guest_texture_pages_registered",
    "TextureHashGuardBlocked": "texture_hash_guard_blocked",
}.items():
    assert name in enum_names, name
    assert labels[enum_names.index(name)] == label, name
    assert name in trace_h and label in trace, name

assert "guest_write_probe=off" in doc
assert "guest_write_probe=guard" in doc
assert "guest_write_probe_mode" in trace
assert "guest_write_probe_texture_pages" in trace
print("Guest JIT write / sticky guard wiring OK (off/count/texture/guard; no default skip).")
