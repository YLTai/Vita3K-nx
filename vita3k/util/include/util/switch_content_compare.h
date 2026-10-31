// Renderer-only content comparison experiment API.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <util/content_hash_memo.h>
namespace switch_validation {
#if defined(__SWITCH__) && defined(VITA3K_SWITCH_TRACELAB)
bool content_compare_enabled();
uint64_t content_hash(const content::Input &input, content::Hash hash);
#else
inline bool content_compare_enabled() { return false; }
inline uint64_t content_hash(const content::Input &input, content::Hash hash) {
    return content::full_digest(input, hash);
}
#endif
} // namespace switch_validation
