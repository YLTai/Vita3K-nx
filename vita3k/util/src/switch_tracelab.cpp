// Vita3K Switch TraceLab: bounded, RAM-first diagnostics for slow SD media.
// SPDX-License-Identifier: GPL-2.0-or-later
#include <util/switch_tracelab.h>

#if defined(__SWITCH__) && defined(VITA3K_SWITCH_TRACELAB)
#include <switch.h>
#include <switch/arm/counter.h>
#include <util/log.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <utility>
#include <vector>
#include <sys/stat.h>

namespace tracelab {
namespace {

constexpr size_t kMetrics = static_cast<size_t>(Metric::Count);
constexpr uint32_t kThreadCapacity = 64;
constexpr uint32_t kEventRingCapacity = 1024;
constexpr uint32_t kNidCapacity = 128;
constexpr uint32_t kFrameCapacity = 32768;
constexpr size_t kTraceCapacity = 65536;
constexpr size_t kWindowCapacity = 32768;

constexpr std::array<const char *, kMetrics> kNames = {
    "jit_run", "jit_recycle", "hle_import", "guest_sched_acquire", "guest_sched_yield",
    "gxm_front_draw", "gxm_state_build", "cmd_pool_hit", "cmd_pool_miss", "cmd_batch",
    "texture_sync", "texture_surface_lookup", "texture_cache", "texture_hash", "texture_upload",
    "index_scan", "vertex_bind", "dcache_clean", "dcache_flush",
    "descriptor_resolve", "descriptor_update", "descriptor_bind",
    "pipeline_lookup", "pipeline_compile", "vk_queue_submit", "vk_queue_present",
    "render_loop", "draw_backend", "collector_poll",
    "texture_hash_unchanged", "texture_hash_changed", "texture_hash_scene_reused",
    "texture_hash_unsafe_skipped", "texture_hash_unsafe_audit",
    "texture_hash_unsafe_audit_mismatch", "texture_hash_unsafe_unverified",
    "texture_hash_probe_new_entry", "texture_hash_probe_eligible",
    "texture_hash_probe_warmup", "texture_hash_probe_changed",
    "texture_hash_probe_no_epoch", "texture_hash_probe_no_baseline",
    "texture_hash_probe_nonmonotonic", "texture_hash_probe_audit_due",
    "texture_hash_probe_import_export", "texture_hash_probe_no_range",
    "texture_hash_probe_invalid_mapping",
    "texture_hash_epoch_delta_1", "texture_hash_epoch_delta_2_7",
    "texture_hash_epoch_delta_8_15", "texture_hash_epoch_delta_16_31",
    "texture_hash_epoch_delta_32plus",
    "texture_scene_boundary", "texture_scene_context_switch",
    "texture_scene_epoch_rewind", "texture_scene_epoch_collision",
    "texture_hash_aggressive_skipped", "texture_hash_aggressive_audits",
    "texture_hash_aggressive_quarantines", "texture_hash_aggressive_unverified"
};
static_assert(kNames.size() == kMetrics);

struct Accumulator {
    std::atomic<uint64_t> calls{0};
    std::atomic<uint64_t> bytes{0};
    std::atomic<uint64_t> samples{0};
    std::atomic<uint64_t> sampled_ticks{0};
    std::atomic<uint64_t> max_ticks{0};
};

// Every slot has one producer; NID sampling is intentionally *not* a text event
// on every HLE exit. It is aggregated here and exported only on normal exit.
struct NidEntry {
    uint32_t nid = 0;
    uint32_t samples = 0;
    uint64_t total_us = 0;
    uint32_t max_us = 0;
    uint32_t over_16ms = 0;
};

struct TraceRecord {
    uint64_t tick;
    uint32_t correlation;
    uint32_t value;
    uint16_t type;
    uint16_t slot;
    uint32_t reserved;
};
static_assert(sizeof(TraceRecord) == 24);

struct FrameSample {
    uint64_t tick;
    uint32_t id;
    uint32_t reserved;
};
static_assert(sizeof(FrameSample) == 16);

// These fields are written by one producer, read by the frontend only after
// the session has stopped. Live counters use atomics for snapshot handoff.
struct alignas(64) ThreadSlot {
    std::atomic<bool> ready{false};
    std::array<Accumulator, kMetrics> metrics{};
    std::atomic<uint32_t> event_write{0};
    std::atomic<uint32_t> event_read{0};
    std::atomic<uint64_t> event_dropped{0};
    std::array<TraceRecord, kEventRingCapacity> events{};
    std::array<NidEntry, kNidCapacity> nids{};
    char label[24]{};
    uint64_t cpu_first_ticks = 0;
    uint64_t cpu_last_ticks = 0;
    uint32_t cpu_good_reads = 0;
    uint32_t cpu_error_code = 0;
    uint32_t last_core = 0;
};

struct Totals {
    uint64_t calls = 0;
    uint64_t bytes = 0;
    uint64_t samples = 0;
    uint64_t sampled_ticks = 0;
    uint64_t max_ticks = 0;
};

struct Window {
    uint64_t elapsed_ticks;
    uint16_t metric;
    uint16_t reserved;
    Totals stats;
};

struct Options {
    Mode mode = Mode::Auto;
    bool legacy_csv = false;
    uint32_t unsafe_hash_skip_interval = 0;
    uint32_t unsafe_recheck_interval = 0;
    // Probe-only classification: zero disables reason counters, not hashing.
    uint32_t hash_probe_interval = 8;
};

struct LocalState {
    ThreadSlot *slot = nullptr;
    uint32_t generation = 0;
    uint32_t rng = 0x9e3779b9U;
    uint64_t next_cpu_check = 0;
    uint64_t previous_frame = 0;
};
thread_local LocalState local{};

std::array<ThreadSlot, kThreadCapacity> slots{};
std::array<Totals, kMetrics> totals{};
std::array<FrameSample, kFrameCapacity> frame_samples{};
std::vector<TraceRecord> trace_records;
std::vector<Window> windows;
std::vector<std::pair<std::string, std::string>> annotations;
std::atomic<Mode> current_mode{Mode::Off};
std::atomic<bool> legacy_csv{false};
std::atomic<uint32_t> unsafe_hash_skip_interval{0};
std::atomic<uint32_t> unsafe_recheck_interval{0};
std::atomic<bool> aggressive_mismatch_seen{false};
std::atomic<bool> aggressive_unverified_seen{false};
uint32_t aggressive_requested_interval = 0;
std::atomic<uint32_t> hash_probe_interval{0};
std::atomic<bool> unsafe_hash_skip_mismatch{false};
std::atomic<bool> unsafe_hash_skip_incomplete{false};
uint32_t unsafe_hash_skip_requested = 0;
std::atomic<uint32_t> generation{0};
std::atomic<uint32_t> next_slot{0};
std::atomic<uint64_t> unavailable_slots{0};
std::atomic<uint64_t> overflow_nids{0};
std::atomic<uint32_t> completed_frames{0};
uint64_t origin_tick = 0;
uint64_t next_poll_tick = 0;
uint64_t session_number = 0;
uint64_t trace_overflow = 0;
uint64_t windows_overflow = 0;
Mode session_mode = Mode::Off;
std::string title_name;

constexpr const char *mode_label(Mode mode) {
    switch (mode) {
    case Mode::Off: return "off";
    case Mode::Light: return "light";
    case Mode::Cpu: return "cpu";
    case Mode::Render: return "render";
    case Mode::Deep: return "deep";
    case Mode::Auto: return "auto";
    }
    return "unknown";
}

Options read_options() {
    Options opt;
    auto *file = std::fopen("sdmc:/switch/vita3k/tracelab.cfg", "rb");
    if (!file)
        return opt;
    char line[128]{};
    while (std::fgets(line, sizeof(line), file)) {
        std::string text(line);
        const auto comment = text.find('#');
        if (comment != std::string::npos) text.resize(comment);
        const auto equal = text.find('=');
        if (equal == std::string::npos) continue;
        std::string key = text.substr(0, equal);
        std::string value = text.substr(equal + 1);
        auto strip = [](std::string &s) {
            const auto first = s.find_first_not_of(" \t\r\n");
            if (first == std::string::npos) { s.clear(); return; }
            const auto last = s.find_last_not_of(" \t\r\n");
            s = s.substr(first, last - first + 1);
        };
        strip(key);
        strip(value);
        if (key == "mode") {
            if (value == "off") opt.mode = Mode::Off;
            else if (value == "light") opt.mode = Mode::Light;
            else if (value == "cpu") opt.mode = Mode::Cpu;
            else if (value == "render") opt.mode = Mode::Render;
            else if (value == "deep") opt.mode = Mode::Deep;
            else if (value == "auto") opt.mode = Mode::Auto;
        } else if (key == "legacy_csv") {
            opt.legacy_csv = value == "1" || value == "true" || value == "on";
        } else if (key == "texture_hash_probe_interval") {
            if (value == "0") opt.hash_probe_interval = 0;
            else if (value == "4") opt.hash_probe_interval = 4;
            else if (value == "8") opt.hash_probe_interval = 8;
            else if (value == "16") opt.hash_probe_interval = 16;
            else if (value == "32") opt.hash_probe_interval = 32;
        } else if (key == "unsafe_texture_hash_recheck_interval") {
            // Alternative count-based experiment, independent of scene gaps.
            // Invalid input disables this deliberately risky setting.
            if (value == "2") opt.unsafe_recheck_interval = 2;
            else if (value == "4") opt.unsafe_recheck_interval = 4;
            else if (value == "8") opt.unsafe_recheck_interval = 8;
            else if (value == "16") opt.unsafe_recheck_interval = 16;
            else if (value == "32") opt.unsafe_recheck_interval = 32;
            else if (value == "64") opt.unsafe_recheck_interval = 64;
            else opt.unsafe_recheck_interval = 0;
        } else if (key == "unsafe_texture_hash_skip_interval") {
            // Only these explicit intervals are accepted. No default
            // performance mode can silently weaken texture validation.
            if (value == "4") opt.unsafe_hash_skip_interval = 4;
            else if (value == "8") opt.unsafe_hash_skip_interval = 8;
            else if (value == "16") opt.unsafe_hash_skip_interval = 16;
            else if (value == "32") opt.unsafe_hash_skip_interval = 32;
            else opt.unsafe_hash_skip_interval = 0;
        }
    }
    std::fclose(file);
    return opt;
}

ThreadSlot *thread_slot() {
    if (current_mode.load(std::memory_order_relaxed) == Mode::Off)
        return nullptr;
    const uint32_t active_generation = generation.load(std::memory_order_acquire);
    if (local.generation != active_generation) {
        local.generation = active_generation;
        local.slot = nullptr;
        local.previous_frame = 0;
        local.next_cpu_check = 0;
        const uint32_t index = next_slot.fetch_add(1, std::memory_order_relaxed);
        if (index >= kThreadCapacity) {
            unavailable_slots.fetch_add(1, std::memory_order_relaxed);
            return nullptr;
        }
        auto &slot = slots[index];
        local.rng ^= index * 0x85ebca6bU + active_generation;
        local.slot = &slot;
        slot.ready.store(true, std::memory_order_release);
    }
    return local.slot;
}

uint32_t us_since(uint64_t tick_delta) {
    const uint64_t hz = armGetSystemTickFreq();
    if (!hz) return 0;
    const uint64_t us = (tick_delta / hz) * 1000000ULL
        + ((tick_delta % hz) * 1000000ULL) / hz;
    return static_cast<uint32_t>(std::min<uint64_t>(us, UINT32_MAX));
}

void publish(ThreadSlot &slot, Event kind, uint32_t correlation, uint32_t value, uint64_t at) {
    const uint32_t write = slot.event_write.load(std::memory_order_relaxed);
    if (write - slot.event_read.load(std::memory_order_acquire) >= kEventRingCapacity) {
        slot.event_dropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const uint16_t slot_id = static_cast<uint16_t>(&slot - slots.data());
    slot.events[write % kEventRingCapacity] = {
        at, correlation, value, static_cast<uint16_t>(kind), slot_id, 0
    };
    slot.event_write.store(write + 1, std::memory_order_release);
}

void sample_frame(uint32_t id, uint64_t now, ThreadSlot &slot) {
    const uint32_t index = completed_frames.fetch_add(1, std::memory_order_relaxed);
    if (index < kFrameCapacity)
        frame_samples[index] = {now, id, 0};
    if (local.previous_frame && now >= local.previous_frame) {
        const uint32_t gap_us = us_since(now - local.previous_frame);
        if (gap_us >= 66000)
            publish(slot, Event::LongFrame, id, gap_us, now);
    }
    local.previous_frame = now;
}

void drain_events() {
    const uint32_t n = std::min(next_slot.load(std::memory_order_acquire), kThreadCapacity);
    for (uint32_t index = 0; index < n; ++index) {
        auto &slot = slots[index];
        if (!slot.ready.load(std::memory_order_acquire)) continue;
        uint32_t read = slot.event_read.load(std::memory_order_relaxed);
        const uint32_t end = slot.event_write.load(std::memory_order_acquire);
        while (read != end) {
            if (trace_records.size() < kTraceCapacity)
                trace_records.push_back(slot.events[read % kEventRingCapacity]);
            else
                ++trace_overflow;
            ++read;
        }
        slot.event_read.store(read, std::memory_order_release);
    }
}

void take_snapshot(uint64_t now) {
    std::array<Totals, kMetrics> sample{};
    const uint32_t n = std::min(next_slot.load(std::memory_order_acquire), kThreadCapacity);
    for (uint32_t index = 0; index < n; ++index) {
        auto &slot = slots[index];
        if (!slot.ready.load(std::memory_order_acquire)) continue;
        for (size_t m = 0; m < kMetrics; ++m) {
            auto &source = slot.metrics[m];
            auto &out = sample[m];
            out.calls += source.calls.exchange(0, std::memory_order_relaxed);
            out.bytes += source.bytes.exchange(0, std::memory_order_relaxed);
            out.samples += source.samples.exchange(0, std::memory_order_relaxed);
            out.sampled_ticks += source.sampled_ticks.exchange(0, std::memory_order_relaxed);
            out.max_ticks = std::max(out.max_ticks,
                source.max_ticks.exchange(0, std::memory_order_relaxed));
        }
    }
    for (size_t m = 0; m < kMetrics; ++m) {
        auto &out = sample[m];
        if (!(out.calls || out.samples || out.bytes)) continue;
        auto &t = totals[m];
        t.calls += out.calls;
        t.bytes += out.bytes;
        t.samples += out.samples;
        t.sampled_ticks += out.sampled_ticks;
        t.max_ticks = std::max(t.max_ticks, out.max_ticks);
        if (windows.size() < kWindowCapacity)
            windows.push_back(Window{now - origin_tick, static_cast<uint16_t>(m), 0, out});
        else
            ++windows_overflow;
    }
}

std::string json_escape(const std::string &s) {
    std::string result;
    result.reserve(s.size() + 2);
    result += '"';
    for (const unsigned char c : s) {
        if (c == '\\' || c == '"') {
            result += '\\';
            result += static_cast<char>(c);
        } else if (c >= 0x20 && c < 0x7f) {
            result += static_cast<char>(c);
        } else {
            result += '_';
        }
    }
    result += '"';
    return result;
}

bool export_io_error = false;
std::FILE *open_buffered(const std::string &filename) {
    auto *file = std::fopen(filename.c_str(), "wb");
    if (file)
        std::setvbuf(file, nullptr, _IOFBF, 65536);
    else
        export_io_error = true;
    return file;
}

void close_checked(std::FILE *file) {
    const bool write_error = std::ferror(file) != 0;
    const int close_result = std::fclose(file);
    if (write_error || close_result != 0)
        export_io_error = true;
}

double to_ms(uint64_t tick_delta) {
    return static_cast<double>(tick_delta) * 1000.0
        / static_cast<double>(armGetSystemTickFreq());
}

bool export_session(uint64_t final_tick) {
    export_io_error = false;
    // No files were opened or written during gameplay. This step intentionally
    // runs only after the emulator has stopped its guest and render workers.
    const std::string parent = "sdmc:/switch/vita3k/logs";
    ::mkdir(parent.c_str(), 0777);
    std::string safe;
    for (const unsigned char c : title_name) {
        if (safe.size() >= 32) break;
        safe += (std::isalnum(c) ? static_cast<char>(c) : '_');
    }
    if (safe.empty()) safe = "unknown";
    const std::string dir = parent + "/tracelab-" + safe + "-"
        + std::to_string(std::time(nullptr)) + "-" + std::to_string(++session_number);
    if (::mkdir(dir.c_str(), 0777) != 0) {
        LOG_ERROR("[tracelab] cannot create export directory {}", dir);
        return false;
    }

    if (auto *file = open_buffered(dir + "/hotspots.csv")) {
        std::fputs("elapsed_ms,thread_slot,metric,calls,bytes,sampled_calls,sampled_wall_ms,estimated_wall_ms,max_sampled_us\n", file);
        for (const auto &w : windows) {
            const auto &v = w.stats;
            const double sampled_ms = to_ms(v.sampled_ticks);
            const double estimate = v.samples
                ? sampled_ms * static_cast<double>(v.calls) / v.samples : 0.0;
            std::fprintf(file, "%.3f,0,%s,%llu,%llu,%llu,%.6f,%.6f,%u\n",
                to_ms(w.elapsed_ticks), kNames[w.metric],
                static_cast<unsigned long long>(v.calls),
                static_cast<unsigned long long>(v.bytes),
                static_cast<unsigned long long>(v.samples),
                sampled_ms, estimate, us_since(v.max_ticks));
        }
        close_checked(file);
    }

    if (auto *file = open_buffered(dir + "/frames.csv")) {
        std::fputs("elapsed_ms,render_frame_id,interval_ms\n", file);
        const uint32_t count = std::min(completed_frames.load(std::memory_order_acquire), kFrameCapacity);
        uint64_t previous = 0;
        for (uint32_t i = 0; i < count; ++i) {
            const auto &frame = frame_samples[i];
            const double interval_ms = previous && frame.tick >= previous
                ? to_ms(frame.tick - previous) : 0.0;
            std::fprintf(file, "%.3f,%u,%.6f\n",
                to_ms(frame.tick - origin_tick), frame.id, interval_ms);
            previous = frame.tick;
        }
        close_checked(file);
    }

    if (auto *file = open_buffered(dir + "/nid-samples.csv")) {
        std::fputs("thread_slot,nid,samples,sampled_wall_ms,max_sampled_us,over_16ms\n", file);
        for (uint32_t index = 0; index < std::min(next_slot.load(), kThreadCapacity); ++index)
            for (const auto &nid : slots[index].nids)
                if (nid.nid && nid.samples)
                    std::fprintf(file, "%u,0x%08X,%u,%.3f,%u,%u\n", index, nid.nid,
                        nid.samples, static_cast<double>(nid.total_us) / 1000.0,
                        nid.max_us, nid.over_16ms);
        close_checked(file);
    }

    if (auto *file = open_buffered(dir + "/threads.csv")) {
        std::fputs("thread_slot,label,cpu_query_ok,cpu_delta_ms,last_cpu_query_rc,last_core\n", file);
        for (uint32_t i = 0; i < std::min(next_slot.load(), kThreadCapacity); ++i) {
            const auto &slot = slots[i];
            if (!slot.ready.load(std::memory_order_acquire)) continue;
            const bool valid = slot.cpu_good_reads >= 2
                && slot.cpu_last_ticks >= slot.cpu_first_ticks;
            const double cpu_ms = valid ? to_ms(slot.cpu_last_ticks - slot.cpu_first_ticks) : 0.0;
            std::fprintf(file, "%u,%s,%u,%.6f,0x%08X,%u\n", i,
                slot.label[0] ? slot.label : "unlabeled", valid ? 1 : 0,
                cpu_ms, slot.cpu_error_code, slot.last_core);
        }
        close_checked(file);
    }

    if (auto *file = open_buffered(dir + "/trace.bin")) {
        const std::array<char, 8> magic{'T','L','A','B','v','1',0,0};
        const uint32_t version = 1;
        const uint32_t entry_size = sizeof(TraceRecord);
        const uint64_t hz = frequency();
        std::fwrite(magic.data(), magic.size(), 1, file);
        std::fwrite(&version, sizeof(version), 1, file);
        std::fwrite(&entry_size, sizeof(entry_size), 1, file);
        std::fwrite(&hz, sizeof(hz), 1, file);
        if (!trace_records.empty())
            std::fwrite(trace_records.data(), sizeof(TraceRecord), trace_records.size(), file);
        close_checked(file);
    }

    if (auto *file = open_buffered(dir + "/summary.json")) {
        std::fputs("{\n  \"metrics\": [\n", file);
        for (size_t i = 0; i < kMetrics; ++i) {
            const auto &v = totals[i];
            std::fprintf(file,
                "%s    {\"metric\":\"%s\",\"calls\":%llu,\"bytes\":%llu,\"samples\":%llu,\"sampled_ticks\":%llu}\n",
                i ? "," : "", kNames[i],
                static_cast<unsigned long long>(v.calls),
                static_cast<unsigned long long>(v.bytes),
                static_cast<unsigned long long>(v.samples),
                static_cast<unsigned long long>(v.sampled_ticks));
        }
        std::fputs("  ]\n}\n", file);
        close_checked(file);
    }

    uint64_t lost_events = trace_overflow + unavailable_slots.load();
    for (const auto &slot : slots)
        lost_events += slot.event_dropped.load(std::memory_order_relaxed);
    const uint64_t frames = completed_frames.load(std::memory_order_relaxed);
    if (auto *file = open_buffered(dir + "/manifest.json")) {
        std::fprintf(file,
            "{\n  \"schema_version\": 2,\n  \"title\": %s,\n"
            "  \"mode\": \"%s\",\n  \"collector\": \"ram_only_until_exit\",\n"
            "  \"legacy_csv\": %s,\n  \"timebase_hz\": %llu,\n"
            "  \"duration_ms\": %.3f,\n  \"gpu_timestamps\": \"unmeasured\",\n",
            json_escape(title_name).c_str(), mode_label(session_mode),
            legacy_csv.load(std::memory_order_relaxed) ? "true" : "false",
            static_cast<unsigned long long>(frequency()), to_ms(final_tick - origin_tick));
        std::fprintf(file,
            "  \"texture_hash_probe_interval\": %u,\n"
            "  \"unsafe_texture_hash_recheck_requested_interval\": %u,\n"
            "  \"unsafe_texture_hash_recheck_final_interval\": %u,\n"
            "  \"unsafe_texture_hash_recheck_mismatch_seen\": %s,\n"
            "  \"unsafe_texture_hash_recheck_unverified_seen\": %s,\n"
            "  \"unsafe_texture_hash_skip_requested_interval\": %u,\n"
            "  \"unsafe_texture_hash_skip_final_interval\": %u,\n"
            "  \"unsafe_texture_hash_audit_mismatch_detected\": %s,\n"
            "  \"unsafe_texture_hash_audit_incomplete\": %s,\n",
            hash_probe_interval.load(std::memory_order_relaxed),
            aggressive_requested_interval,
            unsafe_recheck_interval.load(std::memory_order_relaxed),
            aggressive_mismatch_seen.load(std::memory_order_relaxed) ? "true" : "false",
            aggressive_unverified_seen.load(std::memory_order_relaxed) ? "true" : "false",
            unsafe_hash_skip_requested,
            unsafe_hash_skip_interval.load(std::memory_order_relaxed),
            unsafe_hash_skip_mismatch.load(std::memory_order_relaxed) ? "true" : "false",
            unsafe_hash_skip_incomplete.load(std::memory_order_relaxed) ? "true" : "false");
        std::fprintf(file,
            "  \"frames_recorded\": %u,\n  \"frames_overflow\": %llu,\n"
            "  \"events_dropped\": %llu,\n  \"window_rows_overflow\": %llu,\n"
            "  \"nid_samples_overflow\": %llu,\n  \"trace_bytes\": %llu,\n"
            "  \"trace_path\": \"trace.bin\",\n  \"metadata\": {",
            static_cast<unsigned>(std::min<uint64_t>(frames, kFrameCapacity)),
            static_cast<unsigned long long>(frames > kFrameCapacity ? frames - kFrameCapacity : 0),
            static_cast<unsigned long long>(lost_events),
            static_cast<unsigned long long>(windows_overflow),
            static_cast<unsigned long long>(overflow_nids.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(trace_records.size() * sizeof(TraceRecord)));
        for (size_t i = 0; i < annotations.size(); ++i)
            std::fprintf(file, "%s\n    %s: %s", i ? "," : "",
                json_escape(annotations[i].first).c_str(),
                json_escape(annotations[i].second).c_str());
        std::fputs("\n  }\n}\n", file);
        close_checked(file);
    }
    if (export_io_error) {
        LOG_ERROR("[tracelab] one or more session files failed to write completely: {}", dir);
        return false;
    }
    LOG_INFO("[tracelab] deferred session exported: {} (frames={}, lost_events={}, windows_overflow={})",
        dir, frames, lost_events, windows_overflow);
    return true;
}

} // namespace

uint64_t ticks() { return armGetSystemTick(); }
uint64_t frequency() { return armGetSystemTickFreq(); }
const char *mode_name() { return mode_label(current_mode.load(std::memory_order_relaxed)); }
bool legacy_csv_enabled() { return legacy_csv.load(std::memory_order_relaxed); }
uint32_t unsafe_texture_hash_skip_interval() {
    return unsafe_hash_skip_interval.load(std::memory_order_relaxed);
}
uint32_t unsafe_texture_hash_recheck_interval() {
    return unsafe_recheck_interval.load(std::memory_order_relaxed);
}
void unsafe_texture_hash_recheck_mismatch() {
    // Quarantine is per texture in the renderer. Keep the experiment enabled
    // for unrelated textures, but remember that stale pixels were possible.
    aggressive_mismatch_seen.store(true, std::memory_order_relaxed);
}
void unsafe_texture_hash_recheck_unverified() {
    aggressive_unverified_seen.store(true, std::memory_order_relaxed);
}
uint32_t texture_hash_probe_interval() {
    return hash_probe_interval.load(std::memory_order_relaxed);
}
void unsafe_texture_hash_audit_mismatch() {
    // Disable the experiment globally for the rest of this game session.
    // The already skipped textures may have rendered stale content.
    unsafe_hash_skip_mismatch.store(true, std::memory_order_relaxed);
    unsafe_hash_skip_interval.store(0, std::memory_order_release);
}
void unsafe_texture_hash_audit_incomplete() {
    // No audit is possible after eviction or an invalid guest mapping.
    // A no-mismatch report is not evidence of correctness in that case.
    unsafe_hash_skip_incomplete.store(true, std::memory_order_relaxed);
    unsafe_hash_skip_interval.store(0, std::memory_order_release);
}

bool active(Domain domain) {
    // Light collects cheap counters for all domains, but does not sample time.
    (void)domain;
    return current_mode.load(std::memory_order_relaxed) != Mode::Off;
}

void count(Metric metric, uint64_t bytes) {
    auto *slot = thread_slot();
    if (!slot) return;
    auto &counter = slot->metrics[static_cast<size_t>(metric)];
    counter.calls.fetch_add(1, std::memory_order_relaxed);
    if (bytes) counter.bytes.fetch_add(bytes, std::memory_order_relaxed);
}

Scope::Scope(Metric metric, Domain domain, uint64_t bytes) : metric_(metric) {
    const Mode mode = current_mode.load(std::memory_order_relaxed);
    if (mode == Mode::Off) return;
    auto *slot = thread_slot();
    if (!slot) return;
    slot_ = slot;
    auto &counter = slot->metrics[static_cast<size_t>(metric)];
    counter.calls.fetch_add(1, std::memory_order_relaxed);
    if (bytes) counter.bytes.fetch_add(bytes, std::memory_order_relaxed);

    if (mode == Mode::Light) return;
    if (mode == Mode::Cpu && domain == Domain::Render) return;
    if (mode == Mode::Render && domain == Domain::Cpu) return;
    local.rng ^= local.rng << 13;
    local.rng ^= local.rng >> 17;
    local.rng ^= local.rng << 5;
    const uint32_t mask = mode == Mode::Deep ? 7U : 31U;
    if ((local.rng & mask) == 0)
        begin_ = ticks();
}

Scope::~Scope() {
    if (!begin_ || !slot_) return;
    const uint64_t end = ticks();
    if (end < begin_) return;
    auto *slot = static_cast<ThreadSlot *>(slot_);
    auto &counter = slot->metrics[static_cast<size_t>(metric_)];
    const uint64_t spent = end - begin_;
    counter.sampled_ticks.fetch_add(spent, std::memory_order_relaxed);
    counter.samples.fetch_add(1, std::memory_order_relaxed);
    uint64_t previous = counter.max_ticks.load(std::memory_order_relaxed);
    while (previous < spent
        && !counter.max_ticks.compare_exchange_weak(previous, spent, std::memory_order_relaxed)) {}
}

uint32_t Scope::elapsed_us() const {
    return begin_ ? us_since(ticks() - begin_) : 0;
}

void event(Event kind, uint32_t correlation, uint32_t value) {
    auto *slot = thread_slot();
    if (!slot) return;
    const uint64_t now = ticks();
    if (kind == Event::RenderEnd)
        sample_frame(correlation, now, *slot);
    const Mode mode = current_mode.load(std::memory_order_relaxed);
    // Only deep mode records every batch and render begin. Slow cards cannot
    // sustain unnecessary high-frequency trace output, even when deferred.
    if (mode != Mode::Deep && (kind == Event::BatchConsume || kind == Event::RenderBegin))
        return;
    publish(*slot, kind, correlation, value, now);
}

void nid_sample(uint32_t nid, uint32_t sampled_wall_us) {
    // This is invoked only for randomly timed HLE calls. It intentionally does
    // NOT add a per-call trace event. The NID table has bounded, fixed storage.
    if (!nid) return;
    auto *slot = thread_slot();
    if (!slot) return;
    const uint32_t start = (nid * 0x9e3779b1U) % kNidCapacity;
    for (uint32_t i = 0; i < kNidCapacity; ++i) {
        auto &entry = slot->nids[(start + i) % kNidCapacity];
        if (entry.nid && entry.nid != nid) continue;
        if (!entry.nid) entry.nid = nid;
        ++entry.samples;
        entry.total_us += sampled_wall_us;
        entry.max_us = std::max(entry.max_us, sampled_wall_us);
        if (sampled_wall_us >= 16000) ++entry.over_16ms;
        return;
    }
    overflow_nids.fetch_add(1, std::memory_order_relaxed);
}

void tag_thread(const char *label) {
    if (!label) return;
    auto *slot = thread_slot();
    if (!slot) return;
    std::strncpy(slot->label, label, sizeof(slot->label) - 1);
    slot->label[sizeof(slot->label) - 1] = 0;
}

void cpu_checkpoint() {
    auto *slot = thread_slot();
    if (!slot) return;
    const uint64_t now = ticks();
    if (now < local.next_cpu_check) return;
    local.next_cpu_check = now + frequency();
    u64 total_ticks = 0;
    const Result rc = svcGetInfo(&total_ticks, InfoType_ThreadTickCount,
        CUR_THREAD_HANDLE, TickCountInfo_Total);
    slot->last_core = svcGetCurrentProcessorNumber();
    slot->cpu_error_code = static_cast<uint32_t>(rc);
    if (R_FAILED(rc)) return;
    if (!slot->cpu_good_reads)
        slot->cpu_first_ticks = total_ticks;
    slot->cpu_last_ticks = total_ticks;
    ++slot->cpu_good_reads;
}

void metadata(const char *key, const char *value) {
    if (!key || !value || current_mode.load() == Mode::Off) return;
    // Called only by the frontend at session start.
    if (annotations.size() < 32)
        annotations.emplace_back(key, value);
}

void begin_session(const char *title_id) {
    end_session();
    const auto options = read_options();
    legacy_csv.store(options.legacy_csv, std::memory_order_release);
    // An "off" diagnostic mode cannot silently enable unmonitored
    // inaccurate texture reuse. The experiment also requires a TraceLab
    // build, so normal release binaries always use full hashing.
    const uint32_t aggressive_interval = options.mode == Mode::Off
        ? 0 : options.unsafe_recheck_interval;
    aggressive_requested_interval = aggressive_interval;
    const uint32_t opt_in_interval = options.mode == Mode::Off
        || aggressive_interval != 0 ? 0 : options.unsafe_hash_skip_interval;
    unsafe_hash_skip_requested = opt_in_interval;
    unsafe_hash_skip_mismatch.store(false, std::memory_order_relaxed);
    unsafe_hash_skip_incomplete.store(false, std::memory_order_relaxed);
    unsafe_hash_skip_interval.store(opt_in_interval, std::memory_order_release);
    unsafe_recheck_interval.store(aggressive_interval, std::memory_order_release);
    aggressive_mismatch_seen.store(false, std::memory_order_relaxed);
    aggressive_unverified_seen.store(false, std::memory_order_relaxed);
    hash_probe_interval.store(options.mode == Mode::Off ? 0 : options.hash_probe_interval,
        std::memory_order_release);
    session_mode = options.mode;
    title_name = title_id ? title_id : "unknown";
    if (options.mode == Mode::Off) {
        LOG_INFO("[tracelab] disabled; legacy_csv={}", options.legacy_csv);
        return;
    }

    trace_records.clear();
    windows.clear();
    annotations.clear();
    trace_records.reserve(kTraceCapacity);
    windows.reserve(kWindowCapacity);
    totals.fill({});
    trace_overflow = 0;
    windows_overflow = 0;
    next_slot.store(0, std::memory_order_relaxed);
    completed_frames.store(0, std::memory_order_relaxed);
    unavailable_slots.store(0, std::memory_order_relaxed);
    overflow_nids.store(0, std::memory_order_relaxed);
    for (auto &slot : slots) {
        slot.ready.store(false, std::memory_order_relaxed);
        slot.event_write.store(0, std::memory_order_relaxed);
        slot.event_read.store(0, std::memory_order_relaxed);
        slot.event_dropped.store(0, std::memory_order_relaxed);
        slot.label[0] = '\0';
        slot.cpu_first_ticks = 0;
        slot.cpu_last_ticks = 0;
        slot.cpu_good_reads = 0;
        slot.cpu_error_code = 0;
        slot.last_core = 0;
        for (auto &nid : slot.nids) nid = {};
        for (auto &metric : slot.metrics) {
            metric.calls.store(0, std::memory_order_relaxed);
            metric.bytes.store(0, std::memory_order_relaxed);
            metric.samples.store(0, std::memory_order_relaxed);
            metric.sampled_ticks.store(0, std::memory_order_relaxed);
            metric.max_ticks.store(0, std::memory_order_relaxed);
        }
    }
    origin_tick = ticks();
    next_poll_tick = origin_tick + frequency();
    generation.fetch_add(1, std::memory_order_release);
    current_mode.store(options.mode, std::memory_order_release);
    LOG_WARN("[tracelab] mode={} deferred_io=on legacy_csv={} unsafe_texture_hash_skip_interval={} (writes only on normal exit)",
        mode_name(), options.legacy_csv, opt_in_interval);
    if (opt_in_interval)
        LOG_WARN("[tracelab] UNSAFE texture hash skipping enabled: may briefly render stale textures; audit interval={} scenes", opt_in_interval);
    if (aggressive_interval)
        LOG_WARN("[tracelab] AGGRESSIVE UNSAFE texture hashing: verify once per {} cached uses. A stale texture can render for several frames; changed textures are quarantined individually.", aggressive_interval);
}

void poll() {
    if (current_mode.load(std::memory_order_relaxed) == Mode::Off) return;
    const uint64_t now = ticks();
    if (now < next_poll_tick) return;
    next_poll_tick = now + frequency();
    Scope collector(Metric::CollectorPoll, Domain::Core);
    // RAM only. No formatting, SD writes, fsync or network operations here.
    take_snapshot(now);
    drain_events();
    cpu_checkpoint();
}

void end_session() {
    const Mode was = current_mode.exchange(Mode::Off, std::memory_order_acq_rel);
    if (was == Mode::Off) return;
    const uint64_t ended_at = ticks();
    take_snapshot(ended_at);
    drain_events();
    // Different producers were drained in slot order. Sort only after
    // gameplay ends, outside the performance-critical path.
    std::stable_sort(trace_records.begin(), trace_records.end(),
        [](const TraceRecord &a, const TraceRecord &b) { return a.tick < b.tick; });
    export_session(ended_at);
}

} // namespace tracelab
#endif // __SWITCH__ && VITA3K_SWITCH_TRACELAB
