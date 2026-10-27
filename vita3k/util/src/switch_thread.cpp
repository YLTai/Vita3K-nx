// Vita3K emulator project
// Copyright (C) 2025 Vita3K team
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.

#include <util/switch_thread.h>

#ifdef __SWITCH__

#include <switch.h>

#include <util/log.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <iterator>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <vector>
#include <sys/stat.h>


namespace {

constexpr int32_t IDEAL_CORE_DONT_CARE = -1;
constexpr uint32_t APP_CORE_MASK = 0b0111u;
constexpr uint32_t ALL_CORE_MASK = 0b1111u;
constexpr uint32_t HELPER_CORE_MASK = 0b1000u;

// Vita user priorities run 64 (highest) to 191 (lowest); Horizon runs 0 to 63 the
// same way round. Guest threads map into a band that preserves the game's own
// ordering while staying below the emulator's latency-critical helpers.
constexpr int32_t GUEST_PRIORITY_HIGHEST = 45;
constexpr int32_t GUEST_PRIORITY_LOWEST = 56;
constexpr int32_t VITA_PRIORITY_HIGHEST = 64;
constexpr int32_t VITA_PRIORITY_LOWEST = 191;

// Core 3 uses priority 63 for preemptive multithreading (cores 0..2 use 59).
// Workers allowed on every core must use the lower class so they cannot starve
// Horizon when the scheduler places them on core 3.
constexpr int32_t BACKGROUND_PRIORITY = 63;

} // namespace

void switch_pin_to_app_cores(const char *what) {
    Result rc = svcSetThreadCoreMask(CUR_THREAD_HANDLE, IDEAL_CORE_DONT_CARE, APP_CORE_MASK);
    if (R_SUCCEEDED(rc))
        return;

    rc = svcSetThreadCoreMask(CUR_THREAD_HANDLE, 0, APP_CORE_MASK);
    if (R_FAILED(rc))
        LOG_WARN("Could not restrict {} to cores 0/1/2 (rc=0x{:X}); it may run on the OS-reserved core 3", what, rc);
}


bool switch_pin_to_helper_core(const char *what, const int32_t helper_priority) {
    // Only a build whose NPDM asks for the fourth core may run there; the call
    // simply fails otherwise. Horizon never preempts between equal priorities,
    // so every core-3 tenant carries a distinct one, ordered by how long it runs
    // per wakeup: audio 43, vblank 44, Vulkan wait 45, GXM display 46, render
    // 47. The priority is applied only once the pin succeeded - on the app cores
    // these would outrank the guest itself.
    if (R_SUCCEEDED(svcSetThreadCoreMask(CUR_THREAD_HANDLE, 3, HELPER_CORE_MASK))) {
        const int32_t priority = helper_priority;
        if (R_SUCCEEDED(svcSetThreadPriority(CUR_THREAD_HANDLE, priority)))
            LOG_INFO("[switch] {} runs on the helper core at priority {}", what, priority);
        else
            LOG_INFO("[switch] {} runs on the helper core", what);
        return true;
    }

    // Core 3 unavailable (applet launch): the tick still has to outrank the
    // guest band, or a busy game starves audio and vblank outright under
    // strict priority on the app cores.
    switch_pin_to_app_cores(what);
    if (R_FAILED(svcSetThreadPriority(CUR_THREAD_HANDLE, GUEST_PRIORITY_HIGHEST - 1)))
        LOG_WARN("Could not raise {} above the guest band", what);
    return false;
}

void switch_apply_guest_thread_affinity(const int32_t affinity_mask) {
    // SCE_KERNEL_CPU_MASK_USER_ALL, kept local so this file needs no guest headers.
    constexpr int32_t VITA_USER_CORE_ALL = 0x70000;
    constexpr int32_t VITA_USER_CORE_SHIFT = 16;

    const int32_t requested = affinity_mask & VITA_USER_CORE_ALL;
    if (requested == 0 || requested == VITA_USER_CORE_ALL)
        return;

    const uint32_t host_mask = static_cast<uint32_t>(requested) >> VITA_USER_CORE_SHIFT;
    const int32_t ideal_core = __builtin_ctz(host_mask);
    if (R_SUCCEEDED(svcSetThreadCoreMask(CUR_THREAD_HANDLE, ideal_core, host_mask)))
        LOG_INFO("[switch] guest thread restricted to core mask 0b{:03b} at the game's request", host_mask);
    else
        LOG_WARN("Could not restrict a guest thread to core mask 0b{:03b}", host_mask);
}

int switch_mapped_guest_priority(int vita_priority) {
    if (vita_priority < VITA_PRIORITY_HIGHEST || vita_priority > VITA_PRIORITY_LOWEST)
        return -1;

    constexpr int32_t vita_span = VITA_PRIORITY_LOWEST - VITA_PRIORITY_HIGHEST;
    constexpr int32_t host_span = GUEST_PRIORITY_LOWEST - GUEST_PRIORITY_HIGHEST;
    return GUEST_PRIORITY_HIGHEST
        + ((vita_priority - VITA_PRIORITY_HIGHEST) * host_span + vita_span / 2) / vita_span;
}

void switch_pin_to_hardware_priority(const char *what, const int32_t helper_priority) {
    // These threads stand in for hardware the guest waits on, which never
    // competed for a core on the Vita. Prefer the helper core so their cycles
    // stop costing the guest one of its three.
    if (switch_pin_to_helper_core(what, helper_priority))
        return;

    // Core 3 unavailable (applet launch): fall back to the app cores, raised
    // above the guest band so busy guest threads cannot starve them.
    const int32_t priority = GUEST_PRIORITY_HIGHEST - 1;
    if (R_SUCCEEDED(svcSetThreadPriority(CUR_THREAD_HANDLE, priority)))
        LOG_INFO("[switch] {} raised to priority {}", what, priority);
    else
        LOG_WARN("Could not raise {} above the guest band", what);
}

void switch_allow_helper_core(const char *what) {
    // Permits the helper core without moving the thread there: the scheduler
    // uses core 3's idle time, but the tenants above always preempt it.
    if (R_FAILED(svcSetThreadCoreMask(CUR_THREAD_HANDLE, IDEAL_CORE_DONT_CARE, ALL_CORE_MASK)))
        switch_pin_to_app_cores(what);

    // libnx gives a new thread the priority of its creator, so without this a
    // burst of workers inherits whatever the main thread happens to sit at.
    s32 inherited = 0;
    svcGetThreadPriority(&inherited, CUR_THREAD_HANDLE);
    if (R_SUCCEEDED(svcSetThreadPriority(CUR_THREAD_HANDLE, BACKGROUND_PRIORITY)))
        LOG_INFO("[switch] {} may use any core, at background priority {} (inherited {})", what,
            BACKGROUND_PRIORITY, inherited);
    else
        LOG_WARN("Could not lower {} to background priority", what);
}



void switch_apply_guest_thread_priority(int vita_priority) {
    const int32_t priority = switch_mapped_guest_priority(vita_priority);
    if (priority < 0)
        return; // not a user priority; leave the thread where hbloader put it

    const Result rc = svcSetThreadPriority(CUR_THREAD_HANDLE, static_cast<u32>(priority));
    if (R_FAILED(rc))
        LOG_WARN("Could not set guest thread priority {} (rc=0x{:X})", priority, rc);
}

SwitchCpuBoost::SwitchCpuBoost(const char *what) {
    const Result rc = appletSetCpuBoostMode(ApmCpuBoostMode_FastLoad);
    m_held = R_SUCCEEDED(rc);
    if (m_held)
        LOG_INFO("[switch] CPU boost held for {}", what);
    else
        LOG_WARN("Could not boost the CPU for {} (rc=0x{:X})", what, rc);
}

SwitchCpuBoost::~SwitchCpuBoost() {
    if (m_held)
        appletSetCpuBoostMode(ApmCpuBoostMode_Normal);
}


// CPU profiler: samples kernel thread CPU time, rather than wall-clock
// thread lifetime. Registration scopes ensure handles cannot be queried after
// libnx destroys them. A 1 Hz frontend sampler performs all SD-card I/O.
namespace {
struct CpuProfileEntry {
    uint64_t token = 0;
    Handle handle = INVALID_HANDLE;
    const char *label = "";
    s32 priority = -1;
    u64 affinity = 0;
    std::array<u64, 4> previous{};
    u64 previous_total = 0;
    u64 previous_wall = 0;
    std::array<u64, 4> final{};
    u64 final_total = 0;
    u64 final_wall = 0;
    Result last_core_rc = 0;
    Result last_total_rc = 0;
    u64 self_total = 0;
    u64 self_wall = 0;
    u64 previous_self_total = 0;
    u64 previous_self_wall = 0;
    Result self_rc = 0xFFFFFFFF;
    u32 self_core = 0xFFFFFFFF;
    bool valid = false;
    bool exited = false;
};

struct CpuPhaseStats {
    std::atomic<u64> count{ 0 };
    std::atomic<u64> total_ticks{ 0 };
    std::atomic<u64> max_ticks{ 0 };
    std::atomic<u64> over_60hz{ 0 };
    std::atomic<u64> over_30hz{ 0 };
};

std::mutex cpu_profile_mutex;
std::vector<CpuProfileEntry> cpu_profile_threads;
std::array<CpuPhaseStats, static_cast<size_t>(SwitchCpuProfilePhase::Count)> cpu_profile_phases;
std::atomic<bool> cpu_profile_enabled{ false };
std::atomic<u64> cpu_profile_next_tick{ 0 };
std::FILE *cpu_profile_threads_file = nullptr;
std::FILE *cpu_profile_phases_file = nullptr;
u64 cpu_profile_session_tick = 0;
uint64_t cpu_profile_token = 0;
uint64_t cpu_profile_session_index = 0;
unsigned cpu_profile_sample_index = 0;

bool cpu_profile_read_ticks(
    Handle handle, std::array<u64, 4> &out, u64 &total_out,
    Result &core_rc, Result &total_rc) {
    if (handle == INVALID_HANDLE)
        return false;
    std::array<u64, 4> read{};
    core_rc = 0;
    bool any_core_succeeded = false;
    for (unsigned core = 0; core < 4; ++core) {
        const Result rc = svcGetInfo(&read[core], InfoType_ThreadTickCount,
            handle, core);
        if (R_FAILED(rc)) {
            if (R_SUCCEEDED(core_rc))
                core_rc = rc;
        } else {
            any_core_succeeded = true;
        }
    }
    total_rc = svcGetInfo(&total_out, InfoType_ThreadTickCount,
        handle, TickCountInfo_Total);
    // Preserve errors and raw values instead of treating failure as 0%.
    out = read;
    return any_core_succeeded || R_SUCCEEDED(total_rc);
}

double cpu_profile_to_ms(u64 ticks) {
    return 1000.0 * static_cast<double>(ticks) /
        static_cast<double>(armGetSystemTickFreq());
}

void cpu_profile_reset_phases() {
    for (auto &phase : cpu_profile_phases) {
        phase.count.store(0, std::memory_order_relaxed);
        phase.total_ticks.store(0, std::memory_order_relaxed);
        phase.max_ticks.store(0, std::memory_order_relaxed);
        phase.over_60hz.store(0, std::memory_order_relaxed);
        phase.over_30hz.store(0, std::memory_order_relaxed);
    }
}

void cpu_profile_poll_locked(bool final_sample) {
    if (!cpu_profile_threads_file || !cpu_profile_phases_file)
        return;
    const u64 now = armGetSystemTick();
    if (!final_sample && now < cpu_profile_next_tick.load(std::memory_order_relaxed))
        return;
    const double elapsed_ms = cpu_profile_to_ms(now - cpu_profile_session_tick);

    for (auto &entry : cpu_profile_threads) {
        std::array<u64, 4> current{};
        u64 current_total = 0;
        Result core_rc = 0;
        Result total_rc = 0;
        const bool valid = entry.exited
            ? entry.valid
            : cpu_profile_read_ticks(entry.handle, current, current_total,
                core_rc, total_rc);
        const u64 sample_wall = entry.exited ? entry.final_wall : now;
        if (entry.exited) {
            current = entry.final;
            current_total = entry.final_total;
            core_rc = entry.last_core_rc;
            total_rc = entry.last_total_rc;
        }
        if (!valid)
            continue;
        if (!entry.valid) {
            entry.previous = current;
            entry.previous_total = current_total;
            entry.previous_wall = sample_wall;
            entry.valid = true;
            continue;
        }
        if (sample_wall <= entry.previous_wall)
            continue;
        const u64 window_ticks = sample_wall - entry.previous_wall;
        double core_ms[4]{};
        double core_pct[4]{};
        double total_pct = 0.0;
        const u64 total_delta = current_total >= entry.previous_total
            ? current_total - entry.previous_total : 0;
        const double kernel_total_pct = R_SUCCEEDED(total_rc)
            ? 100.0 * static_cast<double>(total_delta) /
                static_cast<double>(window_ticks) : -1.0;
        u64 self_delta = 0;
        double self_pct = -1.0;
        if (R_SUCCEEDED(entry.self_rc) && entry.self_wall > entry.previous_self_wall) {
            self_delta = entry.self_total >= entry.previous_self_total
                ? entry.self_total - entry.previous_self_total : 0;
            self_pct = 100.0 * static_cast<double>(self_delta) /
                static_cast<double>(entry.self_wall - entry.previous_self_wall);
            entry.previous_self_total = entry.self_total;
            entry.previous_self_wall = entry.self_wall;
        }
        for (unsigned core = 0; core < 4; ++core) {
            const u64 delta = current[core] >= entry.previous[core]
                ? current[core] - entry.previous[core] : 0;
            core_ms[core] = cpu_profile_to_ms(delta);
            core_pct[core] = 100.0 * static_cast<double>(delta) /
                static_cast<double>(window_ticks);
            total_pct += core_pct[core];
        }
        std::fprintf(cpu_profile_threads_file,
            "%.2f,%.2f,%llu,%s,%d,0x%llx,"
            "%.3f,%.3f,%.3f,%.3f,"
            "%.2f,%.2f,%.2f,%.2f,%.2f,%s,"
            "%llu,%llu,%llu,%llu,%llu,%llu,%.4f,"
            "0x%x,0x%x,%llu,%.4f,0x%x,%u,%llu\n",
            elapsed_ms, cpu_profile_to_ms(window_ticks),
            static_cast<unsigned long long>(entry.token), entry.label,
            static_cast<int>(entry.priority),
            static_cast<unsigned long long>(entry.affinity),
            core_ms[0], core_ms[1], core_ms[2], core_ms[3],
            core_pct[0], core_pct[1], core_pct[2], core_pct[3],
            total_pct, entry.exited ? "exit" : "running",
            static_cast<unsigned long long>(current[0]),
            static_cast<unsigned long long>(current[1]),
            static_cast<unsigned long long>(current[2]),
            static_cast<unsigned long long>(current[3]),
            static_cast<unsigned long long>(current_total),
            static_cast<unsigned long long>(total_delta),
            kernel_total_pct,
            static_cast<unsigned>(core_rc),
            static_cast<unsigned>(total_rc),
            static_cast<unsigned long long>(self_delta),
            self_pct,
            static_cast<unsigned>(entry.self_rc),
            static_cast<unsigned>(entry.self_core),
            static_cast<unsigned long long>(armGetSystemTickFreq()));
        entry.previous = current;
        entry.previous_total = current_total;
        entry.previous_wall = sample_wall;
        entry.last_core_rc = core_rc;
        entry.last_total_rc = total_rc;
    }

    cpu_profile_threads.erase(
        std::remove_if(cpu_profile_threads.begin(), cpu_profile_threads.end(),
            [](const CpuProfileEntry &entry) { return entry.exited; }),
        cpu_profile_threads.end());

    static constexpr const char *names[] = {
        "process_batches", "render_frame", "swap_window",
        "lsfg_slot_fence_wait", "lsfg_record_compute", "lsfg_queue_submit",
        "lsfg_present_generated", "lsfg_acquire_image", "lsfg_restore_record",
        "lsfg_present_original", "lsfg_capture_record",
        "lsfg_create", "lsfg_destroy",
        "batch_queue_wait", "batch_sync_wait", "batch_command_execution",
        "vk_queue_mutex_wait", "vk_queue_submit", "vk_queue_present",
        "swapchain_acquire", "swapchain_fence_wait"
    };
    static_assert(std::size(names) == static_cast<size_t>(SwitchCpuProfilePhase::Count));
    for (size_t i = 0; i < cpu_profile_phases.size(); ++i) {
        auto &phase = cpu_profile_phases[i];
        const u64 count = phase.count.exchange(0, std::memory_order_relaxed);
        const u64 total = phase.total_ticks.exchange(0, std::memory_order_relaxed);
        const u64 maximum = phase.max_ticks.exchange(0, std::memory_order_relaxed);
        const u64 slow_60 = phase.over_60hz.exchange(0, std::memory_order_relaxed);
        const u64 slow_30 = phase.over_30hz.exchange(0, std::memory_order_relaxed);
        std::fprintf(cpu_profile_phases_file,
            "%.2f,%s,%llu,%.3f,%.3f,%.3f,%llu,%llu\n",
            elapsed_ms, names[i],
            static_cast<unsigned long long>(count), cpu_profile_to_ms(total),
            count ? cpu_profile_to_ms(total) / count : 0.0,
            cpu_profile_to_ms(maximum),
            static_cast<unsigned long long>(slow_60),
            static_cast<unsigned long long>(slow_30));
    }

    switch_cpu_profile_lsfg_poll(elapsed_ms, final_sample);
    cpu_profile_next_tick.store(now + armGetSystemTickFreq(),
        std::memory_order_release);
    // No per-frame writes. Flush periodically to bound data lost on crashes.
    if (final_sample || (++cpu_profile_sample_index % 3) == 0) {
        std::fflush(cpu_profile_threads_file);
        std::fflush(cpu_profile_phases_file);
    }
}

void cpu_profile_close_locked() {
    // Disable producers before final snapshot; existing samples are retained.
    cpu_profile_enabled.store(false, std::memory_order_release);
    if (cpu_profile_threads_file && cpu_profile_phases_file)
        cpu_profile_poll_locked(true);
    switch_cpu_profile_lsfg_end_session();
    if (cpu_profile_threads_file) {
        std::fclose(cpu_profile_threads_file);
        cpu_profile_threads_file = nullptr;
    }
    if (cpu_profile_phases_file) {
        std::fclose(cpu_profile_phases_file);
        cpu_profile_phases_file = nullptr;
    }
    cpu_profile_next_tick.store(0, std::memory_order_relaxed);
    cpu_profile_reset_phases();
    cpu_profile_threads.erase(
        std::remove_if(cpu_profile_threads.begin(), cpu_profile_threads.end(),
            [](const CpuProfileEntry &entry) { return entry.exited; }),
        cpu_profile_threads.end());
}
} // namespace

SwitchCpuProfileThread::SwitchCpuProfileThread(const char *label) {
    CpuProfileEntry entry{};
    entry.label = label;
    entry.handle = threadGetCurHandle(); // real libnx handle, not a pseudo handle
    if (entry.handle == INVALID_HANDLE)
        return;
    svcGetThreadPriority(&entry.priority, entry.handle);
    s32 preferred_core = -1;
    svcGetThreadCoreMask(&preferred_core, &entry.affinity, entry.handle);
    entry.valid = cpu_profile_read_ticks(entry.handle, entry.previous,
        entry.previous_total, entry.last_core_rc, entry.last_total_rc);
    entry.previous_wall = armGetSystemTick();
    std::lock_guard<std::mutex> lock(cpu_profile_mutex);
    entry.token = ++cpu_profile_token;
    token_ = entry.token;
    cpu_profile_threads.push_back(entry);
}

void SwitchCpuProfileThread::checkpoint() {
    if (!token_ || !cpu_profile_enabled.load(std::memory_order_acquire))
        return;
    const u64 now = armGetSystemTick();
    if (now < next_checkpoint_tick_)
        return;
    next_checkpoint_tick_ = now + armGetSystemTickFreq();

    u64 cpu_ticks = 0;
    const Result rc = svcGetInfo(&cpu_ticks, InfoType_ThreadTickCount,
        CUR_THREAD_HANDLE, TickCountInfo_Total);
    const u32 core = svcGetCurrentProcessorNumber();

    std::lock_guard<std::mutex> lock(cpu_profile_mutex);
    const auto it = std::find_if(cpu_profile_threads.begin(),
        cpu_profile_threads.end(), [this](const CpuProfileEntry &entry) {
            return entry.token == token_;
        });
    if (it == cpu_profile_threads.end())
        return;
    if (R_SUCCEEDED(rc) && it->self_wall == 0) {
        it->previous_self_wall = now;
        it->previous_self_total = cpu_ticks;
    }
    if (R_SUCCEEDED(rc)) {
        it->self_total = cpu_ticks;
        it->self_wall = now;
    }
    it->self_core = core;
    it->self_rc = rc;
}

SwitchCpuProfileThread::~SwitchCpuProfileThread() {
    if (!token_)
        return;
    std::lock_guard<std::mutex> lock(cpu_profile_mutex);
    const auto it = std::find_if(cpu_profile_threads.begin(),
        cpu_profile_threads.end(), [this](const CpuProfileEntry &entry) {
            return entry.token == token_;
        });
    if (it == cpu_profile_threads.end())
        return;
    if (cpu_profile_enabled.load(std::memory_order_relaxed)) {
        // Capture before threadGetCurHandle() becomes invalid at thread exit.
        std::array<u64, 4> end{};
        u64 total = 0;
        Result core_rc = 0;
        Result total_rc = 0;
        if (cpu_profile_read_ticks(it->handle, end, total, core_rc, total_rc)) {
            it->final = end;
            it->final_total = total;
            it->last_core_rc = core_rc;
            it->last_total_rc = total_rc;
            it->final_wall = armGetSystemTick();
            it->exited = true;
            return;
        }
    }
    cpu_profile_threads.erase(it);
}

SwitchCpuProfileSession::SwitchCpuProfileSession(const char *title_id) {
    std::lock_guard<std::mutex> lock(cpu_profile_mutex);
    cpu_profile_close_locked();

    // Sanitize the title ID before incorporating it into a path.
    char safe_id[33]{};
    size_t length = 0;
    if (title_id) {
        for (const char *p = title_id; *p && length < sizeof(safe_id) - 1; ++p) {
            const char c = *p;
            safe_id[length++] = ((c >= '0' && c <= '9') ||
                (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) ? c : '_';
        }
    }
    if (!length)
        std::snprintf(safe_id, sizeof(safe_id), "unknown");

    constexpr const char *dir = "sdmc:/switch/vita3k/logs";
    ::mkdir(dir, 0777);
    const auto epoch = static_cast<long long>(std::time(nullptr));
    const auto index = ++cpu_profile_session_index;
    char cpu_path[192]{};
    char phase_path[192]{};
    std::snprintf(cpu_path, sizeof(cpu_path),
        "%s/cpu-threads-%s-%lld-%llu.csv", dir, safe_id, epoch,
        static_cast<unsigned long long>(index));
    std::snprintf(phase_path, sizeof(phase_path),
        "%s/render-phases-%s-%lld-%llu.csv", dir, safe_id, epoch,
        static_cast<unsigned long long>(index));

    cpu_profile_threads_file = std::fopen(cpu_path, "w");
    cpu_profile_phases_file = std::fopen(phase_path, "w");
    if (!cpu_profile_threads_file || !cpu_profile_phases_file) {
        cpu_profile_close_locked();
        LOG_WARN("[cpu-profile] Could not open CSV files under {}", dir);
        return;
    }
    std::fputs("elapsed_ms,window_ms,thread_id,label,priority,affinity_mask,"
        "core0_ms,core1_ms,core2_ms,core3_ms,"
        "core0_pct,core1_pct,core2_pct,core3_pct,total_pct,state,"
        "raw_core0,raw_core1,raw_core2,raw_core3,raw_total,"
        "delta_total_ticks,kernel_total_pct,core_query_rc,total_query_rc,"
        "self_delta_ticks,self_total_pct,self_query_rc,self_core,tick_frequency\n",
        cpu_profile_threads_file);
    std::fputs("elapsed_ms,phase,count,total_wall_ms,avg_wall_ms,"
        "max_wall_ms,over_16_7ms,over_33_3ms\n", cpu_profile_phases_file);

    cpu_profile_reset_phases();
    cpu_profile_session_tick = armGetSystemTick();
    cpu_profile_next_tick.store(cpu_profile_session_tick +
        armGetSystemTickFreq(), std::memory_order_relaxed);
    cpu_profile_sample_index = 0;
    for (auto &entry : cpu_profile_threads) {
        entry.valid = cpu_profile_read_ticks(entry.handle, entry.previous,
            entry.previous_total, entry.last_core_rc, entry.last_total_rc);
        entry.previous_wall = armGetSystemTick();
    }
    cpu_profile_enabled.store(true, std::memory_order_release);
    switch_cpu_profile_lsfg_begin_session(safe_id, epoch, index);
    LOG_WARN("[cpu-profile] Per-thread CPU log: {}", cpu_path);
    LOG_WARN("[cpu-profile] Render wall-time log: {}", phase_path);
}

SwitchCpuProfileSession::~SwitchCpuProfileSession() {
    std::lock_guard<std::mutex> lock(cpu_profile_mutex);
    cpu_profile_close_locked();
}

void SwitchCpuProfileSession::poll() {
    if (!cpu_profile_enabled.load(std::memory_order_acquire) ||
        armGetSystemTick() < cpu_profile_next_tick.load(std::memory_order_acquire))
        return;
    std::lock_guard<std::mutex> lock(cpu_profile_mutex);
    cpu_profile_poll_locked(false);
}

void switch_cpu_profile_render_phase(
    SwitchCpuProfilePhase phase, uint64_t begin_tick, uint64_t end_tick) {
    if (!cpu_profile_enabled.load(std::memory_order_relaxed) || end_tick < begin_tick)
        return;
    const size_t i = static_cast<size_t>(phase);
    if (i >= cpu_profile_phases.size())
        return;
    auto &p = cpu_profile_phases[i];
    const u64 duration = end_tick - begin_tick;
    p.total_ticks.fetch_add(duration, std::memory_order_relaxed);
    p.count.fetch_add(1, std::memory_order_relaxed);
    if (duration > armGetSystemTickFreq() / 60)
        p.over_60hz.fetch_add(1, std::memory_order_relaxed);
    if (duration > armGetSystemTickFreq() / 30)
        p.over_30hz.fetch_add(1, std::memory_order_relaxed);
    u64 old_max = p.max_ticks.load(std::memory_order_relaxed);
    while (duration > old_max &&
        !p.max_ticks.compare_exchange_weak(old_max, duration,
            std::memory_order_relaxed)) {}
}

#endif
