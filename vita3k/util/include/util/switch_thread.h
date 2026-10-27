// Vita3K emulator project
// Copyright (C) 2025 Vita3K team
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.

#pragma once

#include <cstdint>

// Do not include <switch.h> here: its Mutex and Semaphore names collide with
// Vita3K kernel types. The implementation owns the libnx dependency.
// Diagnostic measurement only: does not alter core affinity or thread priority.
// Sample registered libnx thread CPU ticks on each core while their handles are valid.
class SwitchCpuProfileThread {
public:
    explicit SwitchCpuProfileThread(const char *label);
    ~SwitchCpuProfileThread();
    // Low-frequency, from the instrumented thread itself. Useful when kernel
    // reads made by another core report implausibly small tick increments.
    void checkpoint();
    SwitchCpuProfileThread(const SwitchCpuProfileThread &) = delete;
    SwitchCpuProfileThread &operator=(const SwitchCpuProfileThread &) = delete;
private:
    uint64_t token_ = 0;
    uint64_t next_checkpoint_tick_ = 0;
};

// Per-title session: writes CSV to sdmc:/switch/vita3k/logs/.
class SwitchCpuProfileSession {
public:
    explicit SwitchCpuProfileSession(const char *title_id);
    ~SwitchCpuProfileSession();
    void poll();
    SwitchCpuProfileSession(const SwitchCpuProfileSession &) = delete;
    SwitchCpuProfileSession &operator=(const SwitchCpuProfileSession &) = delete;
};

enum class SwitchCpuProfilePhase : uint8_t {
    Batches, RenderFrame, Present,
    LsfgSlotFenceWait, LsfgRecordFrame, LsfgQueueSubmit,
    LsfgGeneratedPresent, LsfgAcquireImage, LsfgRestoreRecord,
    LsfgOriginalPresent, LsfgCaptureRecord, LsfgCreate, LsfgDestroy,
    Count
};
void switch_cpu_profile_render_phase(SwitchCpuProfilePhase phase, uint64_t begin_tick, uint64_t end_tick);
// Stage values 0..7 correspond to LsfgNxDebugStage in lsfg_bridge.h.
void switch_cpu_profile_lsfg_stage(uint32_t stage, uint64_t begin_tick, uint64_t end_tick);
void switch_cpu_profile_lsfg_state(bool requested, bool compatible, bool passthrough);
void switch_cpu_profile_lsfg_generated(uint32_t frames);
void switch_cpu_profile_lsfg_begin_session(const char *safe_title_id, int64_t epoch, uint64_t session);
void switch_cpu_profile_lsfg_poll(double elapsed_ms, bool final_sample);
void switch_cpu_profile_lsfg_end_session();

#ifdef __SWITCH__
void switch_pin_to_app_cores(const char *what);
// Honour a guest thread's CPU affinity. The Vita's three user cores live in
// bits 16..18 of the mask and map one to one onto the application cores here.
// A game that deliberately pins two threads to one core is relying on them
// never running at the same instant; ignoring that invents concurrency the
// game was never written for. A default or fully open mask changes nothing.
void switch_apply_guest_thread_affinity(int32_t affinity_mask);

// Places a light, periodic, latency-sensitive thread on the core Horizon keeps for
// the OS, when the running NPDM grants it. Never use it for sustained or spinning
// work: starving core 3 hard-locks the console. Falls back to the app cores.
bool switch_pin_to_helper_core(const char *what, int32_t helper_priority);
// Widens a background worker to all four cores so it can absorb the helper
// core's slack, without pinning it there. Its tenants keep a higher priority,
// so this never delays vblank or audio.
void switch_allow_helper_core(const char *what);
// For emulator threads the guest treats as hardware (render, display queue,
// fence processing). Raises them above the guest priority band so busy guest
// threads cannot starve the very work they are waiting on.
void switch_pin_to_hardware_priority(const char *what, int32_t helper_priority);
// Applies a guest thread's Vita priority to the calling Horizon thread. Without
// it every guest thread runs at the process default, level with the render and
// audio threads, so a busy game can hold off the 5 ms audio deadline and the
// game's own priority ordering is ignored entirely.
void switch_apply_guest_thread_priority(int vita_priority);
// The Horizon priority a Vita priority maps to, or -1 if it is not a user
// priority. Horizon schedules strictly by priority, so on the three cores the
// guest gets, a spinning thread starves every lower-priority one outright.
int switch_mapped_guest_priority(int vita_priority);

// Holds Horizon's FastLoad boost for a phase that is CPU bound and draws
// nothing. It raises the CPU clock and drops the GPU to its minimum, so it must
// never cover a phase that renders the game.
class SwitchCpuBoost {
public:
    explicit SwitchCpuBoost(const char *what);
    ~SwitchCpuBoost();
    SwitchCpuBoost(const SwitchCpuBoost &) = delete;
    SwitchCpuBoost &operator=(const SwitchCpuBoost &) = delete;

private:
    bool m_held = false;
};
#else
inline SwitchCpuProfileThread::SwitchCpuProfileThread(const char *) {}
inline SwitchCpuProfileThread::~SwitchCpuProfileThread() {}
inline void SwitchCpuProfileThread::checkpoint() {}
inline SwitchCpuProfileSession::SwitchCpuProfileSession(const char *) {}
inline SwitchCpuProfileSession::~SwitchCpuProfileSession() {}
inline void SwitchCpuProfileSession::poll() {}
inline void switch_cpu_profile_render_phase(SwitchCpuProfilePhase, uint64_t, uint64_t) {}
inline void switch_cpu_profile_lsfg_stage(uint32_t, uint64_t, uint64_t) {}
inline void switch_cpu_profile_lsfg_state(bool, bool, bool) {}
inline void switch_cpu_profile_lsfg_generated(uint32_t) {}
inline void switch_cpu_profile_lsfg_begin_session(const char *, int64_t, uint64_t) {}
inline void switch_cpu_profile_lsfg_poll(double, bool) {}
inline void switch_cpu_profile_lsfg_end_session() {}
inline void switch_pin_to_app_cores(const char *) {}
inline void switch_apply_guest_thread_affinity(int32_t) {}
inline bool switch_pin_to_helper_core(const char *, int32_t) { return false; }
inline void switch_allow_helper_core(const char *) {}
inline void switch_pin_to_hardware_priority(const char *, int32_t) {}
inline void switch_apply_guest_thread_priority(int) {}
inline int switch_mapped_guest_priority(int) { return -1; }

class SwitchCpuBoost {
public:
    explicit SwitchCpuBoost(const char *) {}
};
#endif
