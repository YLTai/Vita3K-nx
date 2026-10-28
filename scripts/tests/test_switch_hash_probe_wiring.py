#!/usr/bin/env python3
"""Guard against untracked per-context scene epochs in Switch texture probing.

This is a source-wiring regression test, not a substitute for gameplay A/B:
it verifies that all known scene boundaries pass through the diagnostic
observer while leaving the original local memo epoch assignments intact.
"""

from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]
vulkan = ROOT / "vita3k/renderer/src/vulkan"
context = (vulkan / "context.cpp").read_text()
scene = (vulkan / "scene.cpp").read_text()
cache = (ROOT / "vita3k/renderer/src/texture/cache.cpp").read_text()
cache_h = (ROOT / "vita3k/renderer/include/renderer/texture_cache.h").read_text()
throttle_h = (ROOT / "vita3k/renderer/include/renderer/texture/hash_throttle.h").read_text()
metrics_h = (ROOT / "vita3k/util/include/util/switch_tracelab.h").read_text()
metric_impl = (ROOT / "vita3k/util/src/switch_tracelab.cpp").read_text()

# Source had two scene boundaries in context.cpp and one in scene.cpp;
# they must all update the diagnostic source identity as well.
assert context.count("note_switch_scene_epoch(") == 2
assert scene.count("note_switch_scene_epoch(") == 1
assert "texture_cache.memo_scene =" not in context
assert "texture_cache.memo_scene =" not in scene
assert "void TextureCache::note_switch_scene_epoch(" in cache
assert "memo_scene = context_scene;" in cache
assert "memo_scene_context = context;" in cache

# No implicit refactor into a global epoch: this step is diagnostic only.
assert "uint64_t memo_scene = 0;" in cache_h
assert "const void *memo_scene_context = nullptr;" in cache_h
assert "classify_hash_throttle(" in throttle_h
assert "should_throttle_hash(" in cache
assert "tracelab::texture_hash_probe_interval()" in cache

enum_match = re.search(r"enum class Metric\s*:\s*uint8_t\s*\{(.*?)\};", metrics_h, re.S)
assert enum_match, "TraceLab Metric enum missing"
enum_clean = re.sub(r"//[^\n]*", "", enum_match.group(1))
enum_names = [x.strip() for x in enum_clean.split(",") if x.strip()]
assert enum_names[-1] == "Count"
enum_names = enum_names[:-1]
assert len(enum_names) < 256

labels_match = re.search(r"constexpr std::array<const char \*, kMetrics> kNames = \{(.*?)\};",
                         metric_impl, re.S)
assert labels_match, "TraceLab metric labels missing"
labels = re.findall(r'"([^"]+)"', labels_match.group(1))
assert len(labels) == len(enum_names), (
    f"Mismatch: {len(enum_names)} metric enums and {len(labels)} labels")

expected = {
    "TextureHashProbeEligible": "texture_hash_probe_eligible",
    "TextureHashProbeWarmup": "texture_hash_probe_warmup",
    "TextureHashProbeChanged": "texture_hash_probe_changed",
    "TextureHashProbeAuditDue": "texture_hash_probe_audit_due",
    "TextureHashProbeNonMonotonic": "texture_hash_probe_nonmonotonic",
    "TextureHashEpochDelta32Plus": "texture_hash_epoch_delta_32plus",
    "TextureSceneContextSwitch": "texture_scene_context_switch",
    "TextureSceneEpochRewind": "texture_scene_epoch_rewind",
    "TextureSceneEpochCollision": "texture_scene_epoch_collision",
}
for name, label in expected.items():
    assert name in enum_names, name
    assert labels[enum_names.index(name)] == label, name

for name in expected:
    if name != "TextureHashEpochDelta32Plus":
        assert name in cache, f"Metric not emitted: {name}"

assert "hash_probe_interval = 8;" in metric_impl
assert "texture_hash_probe_interval" in metric_impl
assert "unsafe_hash_skip_interval = 0;" in metric_impl
print("TraceLab texture hash probe / context epoch wiring OK")
