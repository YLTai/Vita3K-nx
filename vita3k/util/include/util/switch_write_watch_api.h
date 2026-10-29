// Runtime wiring for observational write tables. No API authorizes Hash reuse.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <mutex>
namespace switch_validation {
namespace watch {
struct Token { uint64_t epoch = 0; uint32_t resource = ~0u; };
enum class Access { Unhandled, Complete, Invalid };
}
#if defined(__SWITCH__) && defined(VITA3K_SWITCH_TRACELAB)
class MappingGuard {
    std::unique_lock<std::recursive_mutex> lock_;
public:
    MappingGuard();
};
uint8_t **live_write_table(uint8_t **normal_table);
void live_publish_page(uint8_t **normal_table, uint32_t page, uint8_t *entry) noexcept;
void live_detach_table(uint8_t **normal_table);
watch::Access live_read(uint8_t **normal_table, uint32_t address, void *dst, size_t size);
watch::Access live_write(uint8_t **normal_table, uint32_t address, const void *src, size_t size);
watch::Access live_exclusive(uint8_t **normal_table, uint32_t address, uint64_t value,
    uint64_t expected, size_t size, bool &success);
watch::Token live_hash_begin(uint8_t **normal_table, uint32_t address, const void *pixels,
    uint64_t size, const void *palette, uint64_t palette_size);
void live_hash_end(watch::Token token, uint64_t hash);
#else
class MappingGuard {};
inline uint8_t **live_write_table(uint8_t **) { return nullptr; }
inline void live_publish_page(uint8_t **t, uint32_t page, uint8_t *entry) noexcept { t[page] = entry; }
inline void live_detach_table(uint8_t **) {}
inline watch::Access live_read(uint8_t **, uint32_t, void *, size_t) { return watch::Access::Unhandled; }
inline watch::Access live_write(uint8_t **, uint32_t, const void *, size_t) { return watch::Access::Unhandled; }
inline watch::Access live_exclusive(uint8_t **, uint32_t, uint64_t, uint64_t, size_t, bool &) { return watch::Access::Unhandled; }
inline watch::Token live_hash_begin(uint8_t **, uint32_t, const void *, uint64_t, const void *, uint64_t) { return {}; }
inline void live_hash_end(watch::Token, uint64_t) {}
#endif
} // namespace switch_validation
