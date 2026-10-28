// Vita3K Switch memory validation: one binary, isolated tests and paired runs.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>

namespace switch_validation {

#if defined(__SWITCH__) && defined(VITA3K_SWITCH_TRACELAB)
// Frontend only; begin before TraceLab creates the per-title session.
void begin_session(const char *title_id);
void end_session();
bool enabled();
bool strict_hash();
bool deduplicate_staging_cache();
// Copies all strings immediately. Tests run before any Guest worker starts.
void record(const char *suite, const char *test, const char *status,
    const char *detail, uint64_t elapsed_ticks = 0, uint64_t iterations = 0) noexcept;
// Frontend -> render-thread request. Starts/restarts a complete ABBA/BAAB run.
void request_benchmark();
// Render thread only, exactly once after each completed source render loop.
void on_render_end(uint64_t tick);
// Same render thread as on_render_end; no per-upload atomic bookkeeping.
void note_staging_publish(uint64_t bytes, bool deduplicated);
// Frontend only. Pausing invalidates the current run instead of counting the
// menu/focus gap as a performance sample; resume requires a fresh run.
void frontend_poll(bool paused);
// Called by TraceLab only after all Guest/render workers have stopped.
void write_report(const char *session_directory);

class Session {
public:
    explicit Session(const char *title_id) { begin_session(title_id); }
    ~Session() { end_session(); }
    Session(const Session &) = delete;
    Session &operator=(const Session &) = delete;
};
#else
inline void begin_session(const char *) {}
inline void end_session() {}
inline bool enabled() { return false; }
inline bool strict_hash() { return false; }
inline bool deduplicate_staging_cache() { return false; }
inline void record(const char *, const char *, const char *, const char *, uint64_t = 0, uint64_t = 0) noexcept {}
inline void request_benchmark() {}
inline void on_render_end(uint64_t) {}
inline void note_staging_publish(uint64_t, bool) {}
inline void frontend_poll(bool) {}
inline void write_report(const char *) {}
class Session {
public:
    explicit Session(const char *) {}
};
#endif

} // namespace switch_validation
