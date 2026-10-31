// Private host import + CPU permission capability. No GPU commands are submitted.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#if defined(__SWITCH__) && defined(VITA3K_SWITCH_TRACELAB)
#include <util/switch_heap_fault_probe.h>
#include <util/switch_direct_heap.h>
#include <util/switch_memory_validation.h>
#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <stdexcept>
#include <new>
#include <malloc.h>

namespace switch_validation {
// Called on the existing Vulkan mapping thread, before its first real import.
// A deliberately unsupported RO request is a capability result, not a crash test.
inline void probe_gpu_heap_import(vk::Device device, vk::PhysicalDevice physical,
    const vk::PhysicalDeviceMemoryProperties &properties, vk::BufferUsageFlags usage) {
    if (!heap_fault_probe_enabled()) return;
    const std::lock_guard<std::mutex> probe_lock(direct_heap::scratch_probe_mutex);
    static bool ran = false;
    if (ran) return;
    ran = true;
    if (!heap_fault_checkpoint("gpu_import_permission", true)) {
        record("heap_import", "checkpoint_unavailable", "skip", "previous fault/checkpoint failure; no import probe attempted");
        return;
    }
    struct Marker { ~Marker() { (void)heap_fault_checkpoint("gpu_import_returned", false); } } marker;
    void *backing = nullptr;
    size_t allocation_size = 0;
    vk::Buffer buffer{};
    vk::DeviceMemory memory{};
    bool keep = false;
    bool before_pass = false, imported = false;
    const auto report = [&](const char *name, const HeapPermissionResult &r, bool required) {
        char detail[256];
        std::snprintf(detail, sizeof(detail),
            "RO_rc=0x%x RW_rc=0x%x type=0x%x attr=0x%x query=%u read_only=%u restored_RW=%u; bytes=%zu no GPU work submitted",
            r.read_rc, r.restore_rc, r.before_type, r.before_attr,
            unsigned(r.query_before), unsigned(r.read_only), unsigned(r.writable_after), allocation_size);
        const bool valid = r.query_before && r.writable_after;
        record("heap_import", name, !valid || (required && !r.read_only) ? "fail" : r.read_only ? "pass" : "skip", detail);
        keep |= !r.writable_after;
    };
    try {
        vk::PhysicalDeviceExternalMemoryHostPropertiesEXT host{};
        vk::PhysicalDeviceProperties2 device_properties{};
        device_properties.pNext = &host;
        physical.getProperties2(&device_properties);
        const uint64_t alignment = std::max<uint64_t>(4096, host.minImportedHostPointerAlignment);
        if (!std::has_single_bit(alignment) || alignment > 16 * 1024 * 1024) {
            record("heap_import", "alignment", "fail", "unexpected host-import alignment; no allocation or import attempted");
            return;
        }
        const uint64_t buffer_size = (65536 + alignment - 1) & ~(alignment - 1);
        vk::StructureChain<vk::BufferCreateInfo, vk::ExternalMemoryBufferCreateInfoKHR> info{
            vk::BufferCreateInfo{.size = buffer_size, .usage = usage, .sharingMode = vk::SharingMode::eExclusive},
            vk::ExternalMemoryBufferCreateInfoKHR{.handleTypes = vk::ExternalMemoryHandleTypeFlagBits::eHostAllocationEXT}
        };
        buffer = device.createBuffer(info.get());
        const auto requirements = device.getBufferMemoryRequirements(buffer);
        const uint64_t required = std::max(buffer_size, uint64_t(requirements.size));
        if (required > 16 * 1024 * 1024) throw std::runtime_error("scratch import request unexpectedly large");
        allocation_size = (required + alignment - 1) & ~(alignment - 1);
        backing = memalign(alignment, allocation_size);
        if (!backing) throw std::bad_alloc();
        std::memset(backing, 0x6d, allocation_size);
        const auto before = heap_permission_roundtrip(backing, allocation_size);
        report("before_import", before, true);
        before_pass = before.read_only && before.writable_after;
        if (before_pass) {
            const auto host_properties = device.getMemoryHostPointerPropertiesEXT(vk::ExternalMemoryHandleTypeFlagBits::eHostAllocationEXT, backing);
            const uint32_t bits = host_properties.memoryTypeBits & requirements.memoryTypeBits;
            if (!bits) throw std::runtime_error("host/buffer memory types do not intersect");
            uint32_t type = std::countr_zero(bits);
            for (uint32_t i = 0; i < properties.memoryTypeCount; ++i)
                if ((bits & (1u << i)) && (properties.memoryTypes[i].propertyFlags & vk::MemoryPropertyFlagBits::eHostCached)) { type = i; break; }
            vk::StructureChain<vk::MemoryAllocateInfo, vk::ImportMemoryHostPointerInfoEXT, vk::MemoryAllocateFlagsInfo> allocation{
                vk::MemoryAllocateInfo{.allocationSize = allocation_size, .memoryTypeIndex = type},
                vk::ImportMemoryHostPointerInfoEXT{.handleType = vk::ExternalMemoryHandleTypeFlagBits::eHostAllocationEXT, .pHostPointer = backing},
                vk::MemoryAllocateFlagsInfo{.flags = vk::MemoryAllocateFlagBits::eDeviceAddress}
            };
            memory = device.allocateMemory(allocation.get());
            device.bindBufferMemory(buffer, memory, 0);
            vk::BufferDeviceAddressInfoKHR address_info{.buffer = buffer};
            if (!device.getBufferAddress(address_info))
                throw std::runtime_error("import has no device address");
            imported = true;
            report("while_imported", heap_permission_roundtrip(backing, allocation_size), false);
        }
    } catch (const std::exception &error) {
        record("heap_import", "import_unavailable", "skip", error.what());
    }
    // No queue references exist. Destroying the import unpins before the final
    // restore attempt; never free backing until permissions are actually RW.
    if (buffer) device.destroyBuffer(buffer);
    if (memory) device.freeMemory(memory);
    if (backing) {
        const auto after = heap_permission_roundtrip(backing, allocation_size);
        keep = false; // Earlier failure may have been caused by the now-released pin.
        report("after_release", after, true);
        if (after.writable_after) std::free(backing);
        else heap_fault_retain_allocation();
    }
    record("heap_import", "suite_complete", keep ? "fail" : imported && before_pass ? "pass" : "skip",
        "private import-only capability checked; GPU writes, IPC buffers and Guest Direct Heap remain untested");
}
} // namespace switch_validation
#endif
