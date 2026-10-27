// Bounded, low-overhead Switch TraceLab collector.
// SPDX-License-Identifier: GPL-2.0-or-later
#include <util/switch_tracelab.h>

#if defined(__SWITCH__) && defined(VITA3K_SWITCH_TRACELAB)

#include <switch/arm/counter.h>
#include <util/log.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <limits>
#include <string>
#include <utility>
#include <vector>
#include <sys/stat.h>

namespace tracelab {
namespace {

constexpr uint32_t kMaxThreads = 64;
constexpr uint32_t kEventCapacity = 256;
constexpr uint64_t kTraceLimit = 64ULL * 1024 * 1024;
constexpr size_t kMetrics = static_cast<size_t>(Metric::Count);

constexpr std::array<const char *, kMetrics> kMetricNames = {
    "jit_run", "jit_recycle", "hle_import", "guest_sched_acquire", "guest_sched_yield",
    "gxm_front_draw", "gxm_state_build", "cmd_pool_hit", "cmd_pool_miss", "cmd_batch",
    "texture_sync", "texture_surface_lookup", "texture_cache", "texture_hash", "texture_upload",
    "index_scan", "vertex_bind", "dcache_clean", "dcache_flush",
    "descriptor_resolve", "descriptor_update", "descriptor_bind",
    "pipeline_lookup", "pipeline_compile", "vk_queue_submit", "vk_queue_present",
    "render_loop", "draw_backend", "collector_poll"
};
static_assert(kMetricNames.size() == kMetrics);

struct Accumulator {
    std::atomic<uint64_t> calls{0};
    std::atomic<uint64_t> bytes{0};
    std::atomic<uint64_t> samples{0};
    std::atomic<uint64_t> sampled_ticks{0};
    std::atomic<uint64_t> maximum_ticks{0};
};

struct TraceRecord {
    uint64_t tick;
    uint32_t correlation;
    uint32_t value;
    uint16_t event_type;
    uint16_t thread_slot;
    uint32_t reserved;
};
static_assert(sizeof(TraceRecord) == 24);

struct alignas(64) ThreadSlot {
    std::atomic<bool> ready{false};
    std::atomic<uint32_t> write{0};
    std::atomic<uint32_t> read{0};
    std::atomic<uint64_t> dropped{0};
    std::array<Accumulator, kMetrics> metrics{};
    std::array<TraceRecord, kEventCapacity> events{};
};

struct Totals {
    uint64_t calls = 0;
    uint64_t bytes = 0;
    uint64_t samples = 0;
    uint64_t ticks = 0;
    uint64_t maximum_ticks = 0;
};

std::array<ThreadSlot, kMaxThreads> g_slots{};
std::array<Totals, kMetrics> g_totals{};
std::atomic<uint32_t> g_generation{0};
std::atomic<uint32_t> g_next_slot{0};
std::atomic<uint64_t> g_slots_exhausted{0};
std::atomic<Mode> g_mode{Mode::Off};
Mode g_mode_at_start = Mode::Off;
uint64_t g_events_truncated = 0;
uint64_t g_origin_tick = 0;
uint64_t g_next_poll_tick = 0;
uint64_t g_last_frame_tick = 0;
uint64_t g_trace_bytes = 0;
uint64_t g_session = 0;
uint32_t g_poll_index = 0;
uint64_t g_frame_count = 0;
std::string g_directory;
std::string g_title;
std::vector<std::pair<std::string, std::string>> g_metadata;
std::FILE *g_metrics_file = nullptr;
std::FILE *g_frames_file = nullptr;
std::FILE *g_trace_file = nullptr;

struct LocalState {
    ThreadSlot *slot = nullptr;
    uint32_t generation = 0;
    uint32_t random = 0x2c9277b5;
    uint64_t previous_frame = 0;
};
thread_local LocalState g_local{};

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

Mode load_mode() {
    // Runtime configuration is read once on each game launch. No file access
    // is performed on guest/render worker paths.
    std::FILE *file = std::fopen("sdmc:/switch/vita3k/tracelab.cfg", "rb");
    if (!file)
        return Mode::Auto;
    char line[128]{};
    Mode selected = Mode::Auto;
    while (std::fgets(line, sizeof(line), file)) {
        std::string text(line);
        const auto comment = text.find('#');
        if (comment != std::string::npos)
            text.resize(comment);
        const auto equal = text.find('=');
        if (equal == std::string::npos)
            continue;
        auto key = text.substr(0, equal);
        auto value = text.substr(equal + 1);
        const auto strip = [](std::string &s) {
            const auto b = s.find_first_not_of(" \t\r\n");
            if (b == std::string::npos) { s.clear(); return; }
            const auto e = s.find_last_not_of(" \t\r\n");
            s = s.substr(b, e - b + 1);
        };
        strip(key);
        strip(value);
        if (key != "mode")
            continue;
        if (value == "off") selected = Mode::Off;
        else if (value == "light") selected = Mode::Light;
        else if (value == "cpu") selected = Mode::Cpu;
        else if (value == "render") selected = Mode::Render;
        else if (value == "deep") selected = Mode::Deep;
        else if (value == "auto") selected = Mode::Auto;
    }
    std::fclose(file);
    return selected;
}

ThreadSlot *thread_slot() {
    if (g_mode.load(std::memory_order_relaxed) == Mode::Off)
        return nullptr;
    const uint32_t generation = g_generation.load(std::memory_order_acquire);
    if (g_local.generation != generation) {
        g_local.generation = generation;
        g_local.slot = nullptr;
        g_local.previous_frame = 0;
        const uint32_t index = g_next_slot.fetch_add(1, std::memory_order_relaxed);
        if (index >= kMaxThreads) {
            g_slots_exhausted.fetch_add(1, std::memory_order_relaxed);
            return nullptr;
        }
        auto &slot = g_slots[index];
        g_local.slot = &slot;
        g_local.random ^= index * 0x9e3779b9U + generation;
        slot.ready.store(true, std::memory_order_release);
    }
    return g_local.slot;
}

void publish(ThreadSlot *slot, Event kind, uint32_t correlation, uint32_t value, uint64_t at) {
    const uint32_t next = slot->write.load(std::memory_order_relaxed);
    if (next - slot->read.load(std::memory_order_acquire) >= kEventCapacity) {
        slot->dropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const uint16_t index = static_cast<uint16_t>(slot - g_slots.data());
    slot->events[next % kEventCapacity] = {
        at, correlation, value, static_cast<uint16_t>(kind), index, 0
    };
    slot->write.store(next + 1, std::memory_order_release);
}

uint32_t to_us(uint64_t delta) {
    const uint64_t hz = armGetSystemTickFreq();
    if (!hz)
        return 0;
    const uint64_t us = (delta * 1000000ULL) / hz;
    return static_cast<uint32_t>(std::min<uint64_t>(us, UINT32_MAX));
}

std::string json_string(const std::string &s) {
    std::string result = "\"";
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
    return result + '"';
}

void snapshot(uint64_t now) {
    if (!g_metrics_file)
        return;
    const double elapsed_ms = static_cast<double>(now - g_origin_tick) * 1000.0
        / static_cast<double>(armGetSystemTickFreq());
    for (uint32_t slot_index = 0; slot_index < g_next_slot.load(std::memory_order_acquire)
        && slot_index < kMaxThreads; ++slot_index) {
        auto &slot = g_slots[slot_index];
        if (!slot.ready.load(std::memory_order_acquire))
            continue;
        for (size_t i = 0; i < kMetrics; ++i) {
            auto &metric = slot.metrics[i];
            const uint64_t calls = metric.calls.exchange(0, std::memory_order_relaxed);
            const uint64_t bytes = metric.bytes.exchange(0, std::memory_order_relaxed);
            const uint64_t samples = metric.samples.exchange(0, std::memory_order_relaxed);
            const uint64_t sampled = metric.sampled_ticks.exchange(0, std::memory_order_relaxed);
            const uint64_t maximum = metric.maximum_ticks.exchange(0, std::memory_order_relaxed);
            if (!(calls || bytes || samples))
                continue;
            auto &total = g_totals[i];
            total.calls += calls;
            total.bytes += bytes;
            total.samples += samples;
            total.ticks += sampled;
            total.maximum_ticks = std::max(total.maximum_ticks, maximum);
            const double sampled_ms = static_cast<double>(sampled) * 1000.0
                / static_cast<double>(armGetSystemTickFreq());
            const double estimated_ms = samples
                ? sampled_ms * static_cast<double>(calls) / static_cast<double>(samples) : 0.0;
            std::fprintf(g_metrics_file, "%.3f,%u,%s,%llu,%llu,%llu,%.6f,%.6f,%u\n",
                elapsed_ms, slot_index, kMetricNames[i],
                static_cast<unsigned long long>(calls), static_cast<unsigned long long>(bytes),
                static_cast<unsigned long long>(samples), sampled_ms, estimated_ms, to_us(maximum));
        }
    }
}

void drain_events() {
    for (uint32_t slot_index = 0; slot_index < g_next_slot.load(std::memory_order_acquire)
        && slot_index < kMaxThreads; ++slot_index) {
        auto &slot = g_slots[slot_index];
        if (!slot.ready.load(std::memory_order_acquire))
            continue;
        uint32_t read = slot.read.load(std::memory_order_relaxed);
        const uint32_t published = slot.write.load(std::memory_order_acquire);
        while (read != published) {
            const TraceRecord &record = slot.events[read % kEventCapacity];
            if (g_trace_file && g_trace_bytes + sizeof(record) <= kTraceLimit) {
                if (std::fwrite(&record, sizeof(record), 1, g_trace_file) == 1)
                    g_trace_bytes += sizeof(record);
                else
                    ++g_events_truncated;
            } else {
                ++g_events_truncated;
            }
            if (g_frames_file && record.event_type == static_cast<uint16_t>(Event::RenderEnd)) {
                const double elapsed = static_cast<double>(record.tick - g_origin_tick) * 1000.0
                    / static_cast<double>(armGetSystemTickFreq());
                const double interval = g_last_frame_tick && record.tick >= g_last_frame_tick
                    ? static_cast<double>(record.tick - g_last_frame_tick) * 1000.0
                        / static_cast<double>(armGetSystemTickFreq()) : 0.0;
                std::fprintf(g_frames_file, "%.3f,%u,%.6f\n", elapsed, record.correlation, interval);
                g_last_frame_tick = record.tick;
                ++g_frame_count;
            }
            ++read;
        }
        slot.read.store(read, std::memory_order_release);
    }
}

void close_files() {
    for (auto *f : {g_metrics_file, g_frames_file, g_trace_file})
        if (f) std::fclose(f);
    g_metrics_file = nullptr;
    g_frames_file = nullptr;
    g_trace_file = nullptr;
}

} // namespace

uint64_t ticks() { return armGetSystemTick(); }
uint64_t frequency() { return armGetSystemTickFreq(); }

bool active(Domain domain) {
    const Mode mode = g_mode.load(std::memory_order_relaxed);
    if (mode == Mode::Off) return false;
    if (domain == Domain::Core) return true;
    if (mode == Mode::Light) return false;
    if (mode == Mode::Auto || mode == Mode::Deep) return true;
    return domain == Domain::Cpu ? mode == Mode::Cpu : mode == Mode::Render;
}

void count(Metric metric, uint64_t bytes) {
    auto *slot = thread_slot();
    if (!slot)
        return;
    auto &m = slot->metrics[static_cast<size_t>(metric)];
    m.calls.fetch_add(1, std::memory_order_relaxed);
    if (bytes)
        m.bytes.fetch_add(bytes, std::memory_order_relaxed);
}

void event(Event kind, uint32_t correlation, uint32_t value) {
    auto *slot = thread_slot();
    if (!slot) return;
    const uint64_t now = ticks();
    if (kind == Event::RenderEnd) {
        if (g_local.previous_frame && now >= g_local.previous_frame) {
            const uint32_t interval_us = to_us(now - g_local.previous_frame);
            if (interval_us >= 66000)
                publish(slot, Event::LongFrame, correlation, interval_us, now);
        }
        g_local.previous_frame = now;
    }
    publish(slot, kind, correlation, value, now);
}

Scope::Scope(Metric metric, Domain domain, uint64_t bytes) : metric_(metric) {
    if (!active(domain)) return;
    auto *slot = thread_slot();
    if (!slot) return;
    count(metric, bytes);
    // Independent per-thread pseudorandom sampling avoids aliasing with
    // recurring Draw/SetState sequences. Timestamp queries are sampled.
    g_local.random ^= g_local.random << 13;
    g_local.random ^= g_local.random >> 17;
    g_local.random ^= g_local.random << 5;
    const unsigned mask = g_mode.load(std::memory_order_relaxed) == Mode::Deep ? 3U : 15U;
    if ((g_local.random & mask) == 0)
        begin_ = ticks();
}

Scope::~Scope() {
    if (!begin_) return;
    const uint64_t stop = ticks();
    auto *slot = thread_slot();
    if (!slot || stop < begin_) return;
    auto &m = slot->metrics[static_cast<size_t>(metric_)];
    const uint64_t delta = stop - begin_;
    m.samples.fetch_add(1, std::memory_order_relaxed);
    m.sampled_ticks.fetch_add(delta, std::memory_order_relaxed);
    uint64_t previous = m.maximum_ticks.load(std::memory_order_relaxed);
    while (previous < delta
        && !m.maximum_ticks.compare_exchange_weak(previous, delta, std::memory_order_relaxed)) {}
}

uint32_t Scope::elapsed_us() const {
    return begin_ ? to_us(ticks() - begin_) : 0;
}

const char *mode_name() { return mode_label(g_mode.load(std::memory_order_relaxed)); }

void begin_session(const char *title_id) {
    end_session();
    g_title = title_id ? title_id : "unknown";
    g_directory.clear();
    g_metadata.clear();
    g_totals.fill({});
    g_last_frame_tick = 0;
    g_trace_bytes = 0;
    g_events_truncated = 0;
    g_mode_at_start = Mode::Off;
    g_frame_count = 0;
    g_poll_index = 0;
    g_slots_exhausted.store(0, std::memory_order_relaxed);
    g_next_slot.store(0, std::memory_order_relaxed);
    for (auto &slot : g_slots) {
        slot.ready.store(false, std::memory_order_relaxed);
        slot.write.store(0, std::memory_order_relaxed);
        slot.read.store(0, std::memory_order_relaxed);
        slot.dropped.store(0, std::memory_order_relaxed);
        for (auto &metric : slot.metrics) {
            metric.calls.store(0, std::memory_order_relaxed);
            metric.bytes.store(0, std::memory_order_relaxed);
            metric.samples.store(0, std::memory_order_relaxed);
            metric.sampled_ticks.store(0, std::memory_order_relaxed);
            metric.maximum_ticks.store(0, std::memory_order_relaxed);
        }
    }
    const Mode requested = load_mode();
    if (requested == Mode::Off) {
        LOG_INFO("[tracelab] disabled (mode=off)");
        return;
    }

    std::string safe;
    for (const unsigned char c : g_title) {
        if (safe.size() == 32) break;
        safe += std::isalnum(c) ? static_cast<char>(c) : '_';
    }
    if (safe.empty()) safe = "unknown";
    // First TraceLab run may happen before the legacy CPU profiler creates logs/.
    ::mkdir("sdmc:/switch/vita3k/logs", 0777);
    g_directory = "sdmc:/switch/vita3k/logs/tracelab-" + safe + "-"
        + std::to_string(std::time(nullptr)) + "-" + std::to_string(++g_session);
    if (::mkdir(g_directory.c_str(), 0777) != 0) {
        LOG_WARN("[tracelab] unable to create {}", g_directory);
        return;
    }
    g_metrics_file = std::fopen((g_directory + "/hotspots.csv").c_str(), "wb");
    g_frames_file = std::fopen((g_directory + "/frames.csv").c_str(), "wb");
    g_trace_file = std::fopen((g_directory + "/trace.bin").c_str(), "wb");
    if (!g_metrics_file || !g_frames_file || !g_trace_file) {
        LOG_WARN("[tracelab] unable to open session files under {}", g_directory);
        close_files();
        return;
    }
    std::fputs("elapsed_ms,thread_slot,metric,calls,bytes,sampled_calls,sampled_wall_ms,estimated_wall_ms,max_sampled_us\n", g_metrics_file);
    std::fputs("elapsed_ms,render_frame_id,interval_ms\n", g_frames_file);
    // Header: 8-byte magic, 32-bit format version, 32-bit record size, 64-bit tick frequency.
    const std::array<char, 8> magic = { 'T','L','A','B','v','1',0,0 };
    const uint32_t version = 1;
    const uint32_t record_size = sizeof(TraceRecord);
    const uint64_t tick_frequency = frequency();
    std::fwrite(magic.data(), magic.size(), 1, g_trace_file);
    std::fwrite(&version, sizeof(version), 1, g_trace_file);
    std::fwrite(&record_size, sizeof(record_size), 1, g_trace_file);
    std::fwrite(&tick_frequency, sizeof(tick_frequency), 1, g_trace_file);

    std::fflush(g_trace_file); // header survives even if the application crashes.
    g_mode_at_start = requested;
    g_origin_tick = ticks();
    g_next_poll_tick = g_origin_tick + frequency();
    g_generation.fetch_add(1, std::memory_order_release);
    g_mode.store(requested, std::memory_order_release);
    LOG_INFO("[tracelab] mode={} directory={} (GPU timestamps: unmeasured)",
        mode_name(), g_directory);
}

void metadata(const char *key, const char *value) {
    if (g_mode.load(std::memory_order_relaxed) == Mode::Off || !key || !value)
        return;
    // Only call from the frontend (session configuration), never from worker threads.
    if (g_metadata.size() < 32)
        g_metadata.emplace_back(key, value);
}

void poll() {
    if (!g_metrics_file || g_mode.load(std::memory_order_relaxed) == Mode::Off)
        return;
    const uint64_t now = ticks();
    if (now < g_next_poll_tick) return;
    g_next_poll_tick = now + frequency();
    tracelab::Scope collector(tracelab::Metric::CollectorPoll, tracelab::Domain::Core);
    snapshot(now);
    drain_events();
    if ((++g_poll_index % 3) == 0) {
        std::fflush(g_metrics_file);
        std::fflush(g_frames_file);
        std::fflush(g_trace_file);
    }
}

void end_session() {
    g_mode.store(Mode::Off, std::memory_order_release);
    if (!g_metrics_file) {
        close_files();
        return;
    }
    snapshot(ticks());
    drain_events();
    uint64_t dropped = g_slots_exhausted.load(std::memory_order_relaxed) + g_events_truncated;
    for (const auto &slot : g_slots)
        dropped += slot.dropped.load(std::memory_order_relaxed);

    if (auto *manifest = std::fopen((g_directory + "/manifest.json").c_str(), "wb")) {
        std::fprintf(manifest, "{\n  \"schema_version\": 1,\n  \"title\": %s,\n",
            json_string(g_title).c_str());
        std::fprintf(manifest, "  \"trace_path\": \"trace.bin\",\n  \"timebase_hz\": %llu,\n",
            static_cast<unsigned long long>(frequency()));
        std::fprintf(manifest, "  \"mode\": \"%s\",\n", mode_label(g_mode_at_start));
        std::fprintf(manifest, "  \"gpu_timestamps\": \"unmeasured\",\n");
        std::fprintf(manifest, "  \"frames_recorded\": %llu,\n  \"events_dropped\": %llu,\n",
            static_cast<unsigned long long>(g_frame_count),
            static_cast<unsigned long long>(dropped));
        std::fprintf(manifest, "  \"trace_bytes\": %llu,\n  \"metadata\": {",
            static_cast<unsigned long long>(g_trace_bytes));
        for (size_t i = 0; i < g_metadata.size(); ++i)
            std::fprintf(manifest, "%s\n    %s: %s",
                i ? "," : "", json_string(g_metadata[i].first).c_str(),
                json_string(g_metadata[i].second).c_str());
        std::fputs("\n  }\n}\n", manifest);
        std::fclose(manifest);
    }

    if (auto *summary = std::fopen((g_directory + "/summary.json").c_str(), "wb")) {
        std::fputs("{\n  \"metrics\": [\n", summary);
        for (size_t i = 0; i < kMetrics; ++i) {
            const auto &s = g_totals[i];
            std::fprintf(summary, "%s    {\"metric\":\"%s\",\"calls\":%llu,\"bytes\":%llu,\"samples\":%llu,\"sampled_ticks\":%llu}\n",
                i ? "," : "", kMetricNames[i],
                static_cast<unsigned long long>(s.calls), static_cast<unsigned long long>(s.bytes),
                static_cast<unsigned long long>(s.samples), static_cast<unsigned long long>(s.ticks));
        }
        std::fputs("  ]\n}\n", summary);
        std::fclose(summary);
    }
    close_files();
    LOG_INFO("[tracelab] session saved to {} (frames={}, dropped_events={})",
        g_directory, g_frame_count, dropped);
}

} // namespace tracelab
#endif
