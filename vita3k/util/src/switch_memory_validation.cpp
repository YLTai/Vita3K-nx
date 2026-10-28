// Vita3K Switch memory validation. All measurements stay in RAM until exit.
// SPDX-License-Identifier: GPL-2.0-or-later
#include <util/switch_memory_validation.h>

#if defined(__SWITCH__) && defined(VITA3K_SWITCH_TRACELAB)
#include <util/switch_validation_schedule.h>
#include <util/log.h>
#include <switch/arm/counter.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace switch_validation {
namespace {
constexpr size_t kMaxTests = 256;
constexpr size_t kMaxIntervals = 32768;

struct Options {
    bool enabled = true;
    bool automatic = true;
    uint32_t warmup_seconds = 120;
    uint32_t settle_seconds = 3;
    uint32_t measure_seconds = 20;
};
struct Result {
    std::string suite, name, status, detail;
    uint64_t elapsed_ticks, iterations;
};
struct CloseFile {
    void operator()(FILE *file) const noexcept { std::fclose(file); }
};
struct Interval {
    uint64_t ticks = 0;
    uint32_t phase = 0;
};
struct Phase {
    uint64_t intervals = 0;
    uint64_t ticks = 0;
    uint64_t publish_calls = 0;
    uint64_t publish_bytes = 0;
    uint64_t dedup_calls = 0;
    uint64_t dedup_bytes = 0;
    uint64_t excluded_intervals = 0;
    uint64_t excluded_ticks = 0;
    bool complete = false;
};

Options options;
std::string title;
std::vector<Result> results;
std::mutex results_mutex;
std::atomic<uint64_t> lost_results{0};
std::array<Interval, kMaxIntervals> intervals{};
std::array<Phase, staging_dedup_order.size()> phases{};
std::atomic<bool> is_enabled{false};
std::atomic<bool> dedup_enabled{false};
std::atomic<bool> start_requested{false};
std::atomic<bool> abort_requested{false};
std::atomic<bool> is_paused{false};
// 0=waiting, 1..8=phase, 9=complete, 10=aborted. Only the render thread
// changes a running phase. The frontend reads only these atomic summaries.
std::atomic<uint32_t> public_state{0};
// All four counters belong to the renderer. Frontend resets before workers
// start; export reads only after workers stop. Both variants pay the same
// bookkeeping cost without additional per-upload atomic operations in B.
uint64_t frame_publish_calls = 0, frame_publish_bytes = 0;
uint64_t frame_dedup_calls = 0, frame_dedup_bytes = 0;
uint64_t first_render_tick = 0, run_start_tick = 0, previous_tick = 0;
uint64_t phase_entry_tick = 0;
uint64_t interval_count = 0, overflow = 0;
uint32_t run_id = 0, last_logged_state = ~0U;
bool auto_consumed = false, requested = false, completed = false, aborted = false;

void trim(std::string &value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) { value.clear(); return; }
    const auto last = value.find_last_not_of(" \t\r\n");
    value = value.substr(first, last - first + 1);
}

Options read_options() {
    Options out;
    bool collector_off = false;
    auto *file = std::fopen("sdmc:/switch/vita3k/tracelab.cfg", "rb");
    if (!file) return out;
    char line[256]{};
    while (std::fgets(line, sizeof(line), file)) {
        std::string text(line);
        if (const auto comment = text.find('#'); comment != std::string::npos)
            text.resize(comment);
        const auto equal = text.find('=');
        if (equal == std::string::npos) continue;
        auto key = text.substr(0, equal);
        auto value = text.substr(equal + 1);
        trim(key); trim(value);
        if (key == "mode") {
            collector_off = value == "off";
        } else if (key == "memory_validation") {
            if (value == "off") { out.enabled = false; out.automatic = false; }
            else if (value == "on") { out.enabled = true; out.automatic = false; }
            else if (value == "auto") { out.enabled = true; out.automatic = true; }
        } else if (key == "validation_warmup_seconds" || key == "validation_phase_seconds"
            || key == "validation_settle_seconds") {
            char *end = nullptr;
            errno = 0;
            const unsigned long n = std::strtoul(value.c_str(), &end, 10);
            if (value.empty() || errno || *end || value.front() == '-') continue;
            if (key == "validation_warmup_seconds" && n <= 600) out.warmup_seconds = n;
            if (key == "validation_phase_seconds" && n >= 5 && n <= 120) out.measure_seconds = n;
            if (key == "validation_settle_seconds" && n >= 1 && n <= 30) out.settle_seconds = n;
        }
    }
    std::fclose(file);
    if (collector_off) out.enabled = false;
    return out;
}

void json_string(FILE *file, const std::string &value) {
    std::fputc('"', file);
    for (const unsigned char c : value) {
        switch (c) {
        case '"': std::fputs("\\\"", file); break;
        case '\\': std::fputs("\\\\", file); break;
        case '\n': std::fputs("\\n", file); break;
        case '\r': std::fputs("\\r", file); break;
        case '\t': std::fputs("\\t", file); break;
        default:
            if (c < 32) std::fprintf(file, "\\u%04x", c);
            else std::fputc(c, file);
        }
    }
    std::fputc('"', file);
}

void reset_run(uint64_t tick) {
    phases.fill({});
    interval_count = overflow = 0;
    run_start_tick = tick;
    phase_entry_tick = tick;
    previous_tick = tick;
    ++run_id;
    requested = true;
    completed = aborted = false;
    auto_consumed = true;
    dedup_enabled.store(false, std::memory_order_release);
    public_state.store(1, std::memory_order_release);
}

double percentile(const std::vector<double> &values, double fraction) {
    if (values.empty()) return 0;
    const double position = fraction * (values.size() - 1);
    const size_t lo = static_cast<size_t>(position);
    const size_t hi = std::min(lo + 1, values.size() - 1);
    return values[lo] + (values[hi] - values[lo]) * (position - lo);
}
} // namespace

void begin_session(const char *title_id) {
    end_session();
    options = read_options();
    title = title_id ? title_id : "unknown";
    {
        const std::lock_guard<std::mutex> lock(results_mutex);
        results.clear();
    }
    lost_results.store(0, std::memory_order_relaxed);
    phases.fill({});
    first_render_tick = run_start_tick = previous_tick = phase_entry_tick = 0;
    interval_count = overflow = 0;
    run_id = 0;
    requested = completed = aborted = auto_consumed = false;
    last_logged_state = ~0U;
    frame_publish_calls = frame_publish_bytes = 0;
    frame_dedup_calls = frame_dedup_bytes = 0;
    start_requested.store(false); abort_requested.store(false);
    is_paused.store(false); public_state.store(0);
    is_enabled.store(options.enabled, std::memory_order_release);
    if (options.enabled)
        LOG_INFO("[memory-validation] isolated tests enabled; strict Hash; ABBA/BAAB {}s + {}s settle; L+R+Y starts/restarts; auto={} after {}s",
            options.measure_seconds, options.settle_seconds, options.automatic, options.warmup_seconds);
#ifdef VITA3K_MEMORY_VALIDATION_REVISION
    record("build", "vita3k_revision", "pass", VITA3K_MEMORY_VALIDATION_REVISION);
#else
    record("build", "vita3k_revision", "skip", "build did not supply source revision");
#endif
}

void end_session() {
    // Owner calls this only after workers have stopped and TraceLab exported.
    is_enabled.store(false, std::memory_order_release);
    dedup_enabled.store(false, std::memory_order_release);
}

bool enabled() { return is_enabled.load(std::memory_order_relaxed); }
bool strict_hash() { return enabled(); }
bool deduplicate_staging_cache() { return dedup_enabled.load(std::memory_order_relaxed); }

void record(const char *suite, const char *test, const char *status,
    const char *detail, uint64_t elapsed_ticks, uint64_t iterations_count) noexcept {
    if (!enabled()) return;
    try {
        const std::lock_guard<std::mutex> lock(results_mutex);
        if (results.size() >= kMaxTests) {
            lost_results.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        results.push_back({ suite ? suite : "unknown", test ? test : "unknown",
            status ? status : "fail", detail ? detail : "", elapsed_ticks, iterations_count });
    } catch (...) {
        // Reporting must never bypass SVC/mapping cleanup under memory pressure.
        lost_results.fetch_add(1, std::memory_order_relaxed);
    }
}

void request_benchmark() {
    if (enabled()) start_requested.store(true, std::memory_order_release);
}

void note_staging_publish(uint64_t bytes, bool deduplicated) {
    if (!enabled()) return;
    ++frame_publish_calls;
    frame_publish_bytes += bytes;
    frame_dedup_calls += deduplicated ? 1 : 0;
    frame_dedup_bytes += deduplicated ? bytes : 0;
}

void on_render_end(uint64_t tick) {
    if (!enabled()) return;
    // Attribute publication work only to complete measured frame intervals,
    // excluding a frame that crosses warm-up, phase, or restart boundaries.
    const uint64_t calls = frame_publish_calls, bytes = frame_publish_bytes;
    const uint64_t dedup_calls = frame_dedup_calls, dedup_bytes = frame_dedup_bytes;
    frame_publish_calls = frame_publish_bytes = 0;
    frame_dedup_calls = frame_dedup_bytes = 0;
    if (!first_render_tick) first_render_tick = tick;
    if (abort_requested.exchange(false, std::memory_order_acq_rel)) {
        if (run_start_tick && !completed) {
            aborted = true;
            auto_consumed = true;
            public_state.store(10, std::memory_order_release);
        } else if (!run_start_tick) {
            // A menu during initial loading restarts warm-up on resume. It
            // must not silently consume the automatic run before it starts.
            first_render_tick = tick;
        }
        dedup_enabled.store(false, std::memory_order_release);
    }
    if (is_paused.load(std::memory_order_acquire)) {
        if (!run_start_tick) first_render_tick = 0;
        previous_tick = tick;
        return;
    }
    const uint64_t hz = armGetSystemTickFreq();
    const bool manual = start_requested.exchange(false, std::memory_order_acq_rel);
    const bool automatic = options.automatic && !auto_consumed
        && tick - first_render_tick >= uint64_t(options.warmup_seconds) * hz;
    if (manual || automatic) {
        reset_run(tick);
        return;
    }
    if (!run_start_tick || completed || aborted) return;
    const uint64_t settle = uint64_t(options.settle_seconds) * hz;
    const uint64_t measure = uint64_t(options.measure_seconds) * hz;
    const uint64_t current = tick - run_start_tick;
    const uint64_t previous = previous_tick - run_start_tick;
    const auto position = phase_position(current, settle, measure);
    if (measured_interval(previous, current, settle, measure)
        && previous_tick - phase_entry_tick >= settle) {
        auto &phase = phases[position.index];
        ++phase.intervals;
        phase.ticks += tick - previous_tick;
        phase.publish_calls += calls; phase.publish_bytes += bytes;
        phase.dedup_calls += dedup_calls; phase.dedup_bytes += dedup_bytes;
        if (interval_count < kMaxIntervals)
            intervals[interval_count++] = { tick - previous_tick, position.index };
        else ++overflow;
    } else if (!position.complete) {
        ++phases[position.index].excluded_intervals;
        phases[position.index].excluded_ticks += tick - previous_tick;
    }
    for (uint32_t i = 0; i < position.index && i < phases.size(); ++i)
        phases[i].complete = true;
    if (phase_position(previous, settle, measure).index != position.index)
        phase_entry_tick = tick; // Actual policy change, even after a long stall.
    previous_tick = tick;
    if (position.complete) {
        completed = true;
        dedup_enabled.store(false, std::memory_order_release);
        public_state.store(9, std::memory_order_release);
    } else {
        dedup_enabled.store(position.deduplicate, std::memory_order_release);
        public_state.store(position.index + 1, std::memory_order_release);
    }
}

void frontend_poll(bool paused) {
    if (!enabled()) return;
    const bool was_paused = is_paused.exchange(paused, std::memory_order_acq_rel);
    if (paused && !was_paused) abort_requested.store(true, std::memory_order_release);
    const uint32_t state = public_state.load(std::memory_order_acquire);
    if (state == last_logged_state) return;
    last_logged_state = state;
    if (state >= 1 && state <= staging_dedup_order.size())
        LOG_INFO("[memory-validation] phase {}/8: {} (strict Hash, {}s settle + {}s measurement)",
            state, staging_dedup_order[state - 1] ? "B: noncoherent staging dedup" : "A: original cache maintenance",
            options.settle_seconds, options.measure_seconds);
    else if (state == 9)
        LOG_INFO("[memory-validation] completed; normal exit exports all tests and paired phases in the TraceLab folder");
    else if (state == 10)
        LOG_WARN("[memory-validation] run interrupted by pause/focus/menu; L+R+Y starts a fresh paired run");
}

void write_report(const char *directory) {
    if (!enabled() || !directory) return;
    // Exit can happen while paused, before another render tick consumes it.
    if (abort_requested.load(std::memory_order_acquire) && requested && !completed)
        aborted = true;
    const std::string path = std::string(directory) + "/memory-validation.json";
    std::unique_ptr<FILE, CloseFile> handle(std::fopen(path.c_str(), "wb"));
    auto *file = handle.get();
    if (!file) { LOG_ERROR("[memory-validation] could not open {}", path); return; }
    std::setvbuf(file, nullptr, _IOFBF, 64 * 1024);
    const uint64_t hz = armGetSystemTickFreq();
    std::fputs("{\n  \"schema_version\": 1,\n  \"title\": ", file);
    json_string(file, title);
    std::fprintf(file, ",\n  \"enabled\": true,\n  \"strict_hash\": true,\n  \"hash_skipping\": false,\n"
        "  \"reference_scope\": \"ordinary_texture_cache_existing_hash_footprint\",\n"
        "  \"tests_dropped\": %llu,\n  \"timebase_hz\": %llu,\n  \"tests\": [",
        static_cast<unsigned long long>(lost_results.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(hz));
    {
        const std::lock_guard<std::mutex> lock(results_mutex);
        for (size_t i = 0; i < results.size(); ++i) {
            const auto &r = results[i];
            std::fputs(i ? ",\n    {\"suite\":" : "\n    {\"suite\":", file);
            json_string(file, r.suite);
            std::fputs(",\"name\":", file); json_string(file, r.name);
            std::fputs(",\"status\":", file); json_string(file, r.status);
            std::fputs(",\"detail\":", file); json_string(file, r.detail);
            std::fprintf(file, ",\"elapsed_ticks\":%llu,\"iterations\":%llu}",
                static_cast<unsigned long long>(r.elapsed_ticks), static_cast<unsigned long long>(r.iterations));
        }
    }
    std::fprintf(file,
        "\n  ],\n  \"benchmark\": {\n    \"order\":\"ABBA/BAAB\",\n"
        "    \"auto_start\":%s,\n    \"warmup_seconds\":%u,\n    \"settle_seconds\":%u,\n    \"measure_seconds\":%u,\n"
        "    \"requested\":%s,\n    \"completed\":%s,\n    \"aborted\":%s,\n    \"run_id\":%u,\n"
        "    \"frames_overflow\":%llu,\n    \"phases\":[",
        options.automatic ? "true" : "false", options.warmup_seconds, options.settle_seconds, options.measure_seconds,
        requested ? "true" : "false", completed ? "true" : "false", aborted ? "true" : "false", run_id,
        static_cast<unsigned long long>(overflow));
    for (size_t i = 0; i < phases.size(); ++i) {
        const auto &p = phases[i];
        std::vector<double> milliseconds;
        milliseconds.reserve(std::min<uint64_t>(p.intervals, interval_count));
        for (size_t n = 0; n < interval_count; ++n)
            if (intervals[n].phase == i) milliseconds.push_back(1000.0 * intervals[n].ticks / hz);
        std::sort(milliseconds.begin(), milliseconds.end());
        const double duration_ms = 1000.0 * p.ticks / hz;
        std::fprintf(file,
            "%s\n      {\"index\":%zu,\"variant\":\"%s\",\"complete\":%s,\"measured_intervals\":%llu,"
            "\"duration_ms\":%.6f,\"loops_per_second\":%.6f,\"p50_ms\":%.6f,\"p95_ms\":%.6f,\"p99_ms\":%.6f,"
            "\"publish_calls\":%llu,\"publish_bytes\":%llu,\"dedup_calls\":%llu,\"dedup_bytes\":%llu,"
            "\"excluded_intervals\":%llu,\"excluded_duration_ms\":%.6f}",
            i ? "," : "", i, staging_dedup_order[i] ? "dedup" : "baseline", p.complete ? "true" : "false",
            static_cast<unsigned long long>(p.intervals), duration_ms,
            p.ticks ? double(p.intervals) * hz / p.ticks : 0,
            percentile(milliseconds, 0.50), percentile(milliseconds, 0.95), percentile(milliseconds, 0.99),
            static_cast<unsigned long long>(p.publish_calls), static_cast<unsigned long long>(p.publish_bytes),
            static_cast<unsigned long long>(p.dedup_calls), static_cast<unsigned long long>(p.dedup_bytes),
            static_cast<unsigned long long>(p.excluded_intervals), 1000.0 * p.excluded_ticks / hz);
    }
    std::fputs("\n    ]\n  }\n}\n", file);
    const bool write_error = std::ferror(file) != 0;
    const bool close_error = std::fclose(handle.release()) != 0;
    if (write_error || close_error)
        LOG_ERROR("[memory-validation] incomplete report {}", path);
    else
        LOG_INFO("[memory-validation] wrote {}", path);
}
} // namespace switch_validation
#endif
