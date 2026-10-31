// Included by renderer.cpp only. Keep the original materializer/fallback paths.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <renderer/vulkan/state.h>
#include <mem/functions.h>
#include <util/log.h>
#include <util/switch_deferred_import.h>
#include <util/switch_direct_heap.h>
#include <stdexcept>
#ifdef __SWITCH__
#include <switch/arm/counter.h>
#endif

namespace renderer::vulkan {
#ifdef __SWITCH__
static bool switch_range_alias_is_flat(const MemState &mem, Address addr, uint32_t size);
#endif

bool VKState::map_memory(MemState &memory, Ptr<void> address, uint32_t size) {
#if defined(__SWITCH__) && defined(VITA3K_SWITCH_TRACELAB)
    namespace lazy = switch_validation::deferred_import;
    if (mapping_method == MappingMethod::ExernalHost && switch_validation::direct_heap::state.active.load()
        && lazy::registry.enabled() && address.address() && size
        && !(address.address() & 4095) && !(size & 4095)
        && is_valid_addr_range_size(memory, address.address(), size)
        && switch_range_alias_is_flat(memory, address.address(), size)) {
        const auto host = reinterpret_cast<uintptr_t>(address.get(memory));
        const auto existing = mapped_memories.find(address.address());
        if (existing != mapped_memories.end()) {
            const auto old = lazy::registry.get(existing->second.switch_lazy_token);
            if (old.token && old.host == host && old.bytes == size
                && (old.phase == lazy::Phase::Pending || old.phase == lazy::Phase::Imported || old.phase == lazy::Phase::Fallback))
                return true;
            LOG_ERROR("Conflicting deferred mapping at {}", address.address());
            lazy::registry.fail(existing->second.switch_lazy_token);
            return false;
        }
        const auto token = lazy::registry.reserve(reinterpret_cast<uintptr_t>(this), address.address(), host, size);
        if (token) {
            try {
                // A logical registration only: NO Vulkan object, device address,
                // CPU copy, cache flush or permission change is created here.
                auto [it, inserted] = mapped_memories.emplace(address.address(), MappedMemory{
                    address.address(), ExternalBuffer{ {}, reinterpret_cast<void *>(host) }, {}, size, 0 });
                if (!inserted) throw std::runtime_error("deferred map insertion conflict");
                it->second.switch_lazy_token = token;
                return true;
            } catch (...) { lazy::registry.release(token); throw; }
        }
        // Bounded diagnostic capacity exhausted: preserve the old eager behavior.
    }
#endif
    return map_memory_eager(memory, address, size);
}

#ifdef __SWITCH__
bool VKState::switch_ensure_mapping(Address base, uint32_t uses) {
    namespace lazy = switch_validation::deferred_import;
    auto it = mapped_memories.find(base);
    if (it == mapped_memories.end()) return false;
    auto &current = it->second;
    if (current.buffer) {
        // At most one ledger update per new use kind, not one mutex per draw.
        if (current.switch_lazy_token && (current.switch_lazy_uses & uses) != uses) {
            lazy::registry.note_use(current.switch_lazy_token, uses);
            current.switch_lazy_uses |= uses;
        }
        return true;
    }
    const auto token = current.switch_lazy_token;
    const auto size = current.size;
    if (!token || !mem || !is_valid_addr_range_size(*mem, base, size)
        || !switch_range_alias_is_flat(*mem, base, size)
        || !lazy::registry.claim(token, reinterpret_cast<uintptr_t>(this),
            reinterpret_cast<uintptr_t>(Ptr<void>(base).get(*mem)), size)) {
        lazy::registry.fail(token);
        throw std::runtime_error("Deferred GPU mapping lost its registered backing");
    }
    const uint64_t begin = armGetSystemTick();
    try {
        // Use exactly the original import, memory type, binding, address and
        // DoubleBuffer fallback. Materialize the WHOLE mapping: shader bounds
        // and arbitrary accesses within that mapping remain unchanged.
        if (!map_memory_eager(*mem, Ptr<void>(base), size))
            throw std::runtime_error("Deferred GPU mapping materialization failed");
        auto &created = mapped_memories.at(base);
        created.switch_lazy_token = token;
        created.switch_lazy_uses = uses;
        if (!created.buffer || !created.buffer_address)
            throw std::runtime_error("Deferred GPU mapping has no buffer/device address");
        const bool imported = std::holds_alternative<ExternalBuffer>(created.buffer_impl);
        if (!lazy::registry.publish(token, imported, uses, armGetSystemTick() - begin))
            throw std::runtime_error("Deferred GPU mapping publication failed");
        return true;
    } catch (...) { lazy::registry.fail(token); throw; }
}
#endif
} // namespace renderer::vulkan
