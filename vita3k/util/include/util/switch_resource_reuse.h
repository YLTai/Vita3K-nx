// Production bridge; only Switch TraceLab supplies the implementation.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
namespace switch_validation {
using ResourceHash = uint64_t (*)(const void *, size_t);
#if defined(__SWITCH__) && defined(VITA3K_SWITCH_TRACELAB)
bool resource_reuse_enabled();
uint64_t resource_texture_hash(uint8_t **table, uint32_t address, const void *pixels,
    uint64_t size, const void *palette, uint64_t palette_size, ResourceHash hash);
void resource_gpu_write(const void *host, uint64_t size) noexcept;
void resource_backend(bool supported) noexcept;
void resource_deny(uint32_t reason) noexcept;
#else
inline bool resource_reuse_enabled() { return false; }
inline uint64_t resource_texture_hash(uint8_t **, uint32_t, const void *p, uint64_t n,
    const void *q, uint64_t m, ResourceHash hash) { return (p && n ? hash(p,n) : 0) ^ (q && m ? hash(q,m) : 0); }
inline void resource_gpu_write(const void *, uint64_t) noexcept {}
inline void resource_backend(bool) noexcept {}
inline void resource_deny(uint32_t) noexcept {}
#endif
} // namespace switch_validation
