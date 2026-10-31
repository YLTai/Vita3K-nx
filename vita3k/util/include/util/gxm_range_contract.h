// Synchronous GXM frontend outputs; NOT GPU/async write coverage.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <util/jit_epoch_gate.h>

namespace switch_validation::epoch {
template<class Context, class Memory, class ContextType>
bool declare_gxm_context(Context *context, const Memory &mem,
    std::initializer_list<const char *> roots, bool commands,
    ContextType immediate_type, uint64_t command_bytes, HostSpan extra = {}) {
    if (!enabled.load(std::memory_order_acquire) || !context) return false;
    HostSpan ring;
    if (commands) {
        // Deferred allocation may invoke arbitrary Guest callbacks. It must
        // remain unknown rather than inherit the immediate-context contract.
        if (context->state.type != immediate_type) return false;
        const uint64_t count = context->command_allocator_size;
        if (!command_bytes || count > UINT64_MAX / command_bytes) return false;
        const uint64_t bytes = count * command_bytes;
        if (bytes) {
            const uint64_t first = context->alloc_space.address();
            const uint64_t end = context->alloc_space_end.address();
            if (!first || end < first || bytes > end - first) return false;
            auto *host = context->alloc_space.get(mem);
            if (!host) return false;
            ring = {host, bytes};
            if (!ring.valid()) return false;
        }
    }
    // Immediate commands can be constructed inside the Guest VDM allocation,
    // not only in the host pool. The full ring also includes previous next links.
    if (!extra.valid()) return false;
    return declare_outputs(roots, {{context, sizeof(*context)}, ring, extra});
}
} // namespace switch_validation::epoch
