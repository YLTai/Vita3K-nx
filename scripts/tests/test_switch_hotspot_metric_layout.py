#!/usr/bin/env python3
"""Check metric name/enum correspondence and build-mode isolation without a Switch SDK."""

from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]
header = (ROOT / "vita3k/util/include/util/switch_thread.h").read_text()
impl = (ROOT / "vita3k/util/src/switch_thread.cpp").read_text()
scene = (ROOT / "vita3k/renderer/src/vulkan/scene.cpp").read_text()
cmake = (ROOT / "vita3k/renderer/CMakeLists.txt").read_text()

enum = re.search(r"enum class SwitchCpuFineMetric\s*:\s*uint8_t\s*\{(.*?)\};", header, re.S)
assert enum, "fine metric enum missing"
clean = re.sub(r"//[^\n]*", "", enum.group(1))
values = [value.strip() for value in clean.split(",") if value.strip()]
assert values[-1] == "Count"
metrics = values[:-1]
assert len(metrics) < 256, "metric IDs must fit the profiler's uint8_t type"

names = re.search(r"static constexpr const char \*fine_names\[\]\s*=\s*\{(.*?)\};", impl, re.S)
assert names, "fine names missing"
labels = re.findall(r'"([^"]+)"', names.group(1))
assert len(labels) == len(metrics), (
    f"fine metric names/IDs out of sync: {len(labels)} labels, {len(metrics)} values"
)

expected = {
    "DrawSkippedMask": "draw_skipped_mask",
    "DrawSkippedPipeline": "draw_skipped_pipeline",
    "TextureSurfaceLookup": "texture_surface_lookup",
    "TextureCacheOperation": "texture_cache_operation",
    "TextureDescriptorState": "texture_descriptor_state",
    "DrawDescriptorResolve": "draw_descriptor_resolve",
    "DrawDescriptorUpdate": "draw_descriptor_update",
    "DrawDescriptorBind": "draw_descriptor_bind",
    "DrawIndexData": "draw_index_data",
    "DrawVertexData": "draw_vertex_data",
    "DescBindSkipped": "desc_bind_skipped",
    "DescBindPartial": "desc_bind_partial",
    "DescBindFull": "desc_bind_full",
    "DescBindUnconditional": "desc_bind_unconditional",
    "DescPoolGrow": "desc_pool_grow",
    "TextureCacheFallback": "texture_cache_fallback",
    "IndexRangeScanned": "index_range_scanned",
    "IndexRangeReused": "index_range_reused",
}
for name, label in expected.items():
    assert labels[metrics.index(name)] == label, f"bad label for {name}"

skip = metrics.index("DrawSkippedMask")
start_timed = metrics.index("TextureSurfaceLookup")
start_counters = metrics.index("DescBindSkipped")
assert start_timed > skip and start_counters > metrics.index("DrawVertexData")
assert "i == static_cast<size_t>(SwitchCpuFineMetric::DrawSkippedMask)" in impl
assert "i == static_cast<size_t>(SwitchCpuFineMetric::DrawSkippedPipeline)" in impl
assert "i >= static_cast<size_t>(SwitchCpuFineMetric::DescBindSkipped)" in impl

# The OFF reference must still bind all descriptor sets and dynamic offsets.
assert "option(SWITCH_DESCRIPTOR_BIND_CACHE" in cmake
assert "VITA3K_SWITCH_DISABLE_DESCRIPTOR_BIND_CACHE=1" in cmake
assert "defined(VITA3K_SWITCH_DISABLE_DESCRIPTOR_BIND_CACHE)" in scene
assert "DescBindUnconditional" in scene
assert "dynamic_offset_count, dynamic_offsets.data()" in scene

print(f"Switch hot-path metric layout and A/B guard: {len(metrics)} IDs OK")
