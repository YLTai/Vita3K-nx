// Vita3K Switch TraceLab: low-overhead, per-session performance telemetry.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>

namespace tracelab {

enum class Domain : uint8_t { Core, Cpu, Render };
enum class Mode : uint8_t { Off, Light, Cpu, Render, Deep, Auto };
enum class Metric : uint8_t {
    JitRun, JitRecycle, HleImport, GuestSchedAcquire, GuestSchedYield,
    GxmFrontDraw, GxmStateBuild, CmdPoolHit, CmdPoolMiss, CmdBatch,
    TextureSync, TextureSurfaceLookup, TextureCache, TextureHash, TextureUpload,
    IndexScan, VertexBind, CacheClean, CacheFlush,
    DescriptorResolve, DescriptorUpdate, DescriptorBind,
    PipelineLookup, PipelineCompile, QueueSubmit, QueuePresent,
    RenderLoop, DrawBack, CollectorPoll,
    TextureHashUnchanged, TextureHashChanged, TextureHashSceneReused,
    TextureHashUnsafeSkipped, TextureHashUnsafeAudit,
    TextureHashUnsafeAuditMismatch, TextureHashUnsafeUnverified,
    TextureHashProbeNewEntry, TextureHashProbeEligible,
    TextureHashProbeWarmup, TextureHashProbeChanged,
    TextureHashProbeNoEpoch, TextureHashProbeNoBaseline,
    TextureHashProbeNonMonotonic, TextureHashProbeAuditDue,
    TextureHashProbeImportExport, TextureHashProbeNoRange,
    TextureHashProbeInvalidMapping,
    TextureHashEpochDelta1, TextureHashEpochDelta2To7,
    TextureHashEpochDelta8To15, TextureHashEpochDelta16To31,
    TextureHashEpochDelta32Plus,
    TextureSceneBoundary, TextureSceneContextSwitch,
    TextureSceneEpochRewind, TextureSceneEpochCollision,
    TextureHashAggressiveSkipped, TextureHashAggressiveAudits,
    TextureHashAggressiveQuarantines, TextureHashAggressiveUnverified,
    Count
};
enum class Event : uint16_t {
    RenderBegin = 1, RenderEnd, GxmNewFrame, BatchConsume,
    HleNidSample, LongFrame, PipelineCompiled, JitCacheRecycle,
    TextureHashUnsafeMismatch
};

#if defined(__SWITCH__) && defined(VITA3K_SWITCH_TRACELAB)

uint64_t ticks();
uint64_t frequency();
bool active(Domain domain);
void count(Metric metric, uint64_t bytes = 0);
void event(Event event, uint32_t correlation = 0, uint32_t value = 0);
void nid_sample(uint32_t nid, uint32_t sampled_wall_us);
void tag_thread(const char *label);
void cpu_checkpoint();
bool legacy_csv_enabled();
// EXPERIMENTAL ONLY: cross-scene skips have no complete write tracking on
// Horizon; explicitly opt in via tracelab.cfg and expect possible stale texels.
uint32_t unsafe_texture_hash_skip_interval();
// Aggressive, per-texture reuse-count experiment (explicit opt-in only).
// Unlike the historical scene-epoch throttle, this is designed to reach
// heavy scenes with >8 scene boundaries per rendered frame.
uint32_t unsafe_texture_hash_recheck_interval();
void unsafe_texture_hash_recheck_mismatch();
void unsafe_texture_hash_recheck_unverified();
// Independent, read-only interval used to classify would-be Hash skips.
// Diagnostic classifications never change cache/hash validity.
uint32_t texture_hash_probe_interval();
void unsafe_texture_hash_audit_mismatch();
void unsafe_texture_hash_audit_incomplete();
void begin_session(const char *title_id);
void end_session();
void poll();
void metadata(const char *key, const char *value);
const char *mode_name();

class Session {
public:
    explicit Session(const char *title_id) { begin_session(title_id); }
    ~Session() { end_session(); }
    Session(const Session &) = delete;
    Session &operator=(const Session &) = delete;
};

class Scope {
public:
    Scope(Metric metric, Domain domain, uint64_t bytes = 0);
    ~Scope();
    Scope(const Scope &) = delete;
    Scope &operator=(const Scope &) = delete;
    bool sampled() const { return begin_ != 0; }
    uint32_t elapsed_us() const;
private:
    Metric metric_;
    uint64_t begin_ = 0;
    void *slot_ = nullptr;
};

#else

inline uint64_t ticks() { return 0; }
inline uint64_t frequency() { return 1; }
inline bool active(Domain) { return false; }
inline void count(Metric, uint64_t = 0) {}
inline void event(Event, uint32_t = 0, uint32_t = 0) {}
inline void nid_sample(uint32_t, uint32_t) {}
inline void tag_thread(const char *) {}
inline void cpu_checkpoint() {}
inline bool legacy_csv_enabled() { return true; }
inline uint32_t unsafe_texture_hash_skip_interval() { return 0; }
inline uint32_t unsafe_texture_hash_recheck_interval() { return 0; }
inline void unsafe_texture_hash_recheck_mismatch() {}
inline void unsafe_texture_hash_recheck_unverified() {}
inline uint32_t texture_hash_probe_interval() { return 0; }
inline void unsafe_texture_hash_audit_mismatch() {}
inline void unsafe_texture_hash_audit_incomplete() {}
inline void begin_session(const char *) {}
inline void end_session() {}
inline void poll() {}
inline void metadata(const char *, const char *) {}
inline const char *mode_name() { return "off"; }

class Session {
public:
    explicit Session(const char *) {}
};

class Scope {
public:
    Scope(Metric, Domain, uint64_t = 0) {}
    bool sampled() const { return false; }
    uint32_t elapsed_us() const { return 0; }
};

#endif
} // namespace tracelab
