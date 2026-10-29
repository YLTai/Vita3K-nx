// Vita3K Switch memory validation. All measurements stay in RAM until exit.
// SPDX-License-Identifier: GPL-2.0-or-later
#include <util/switch_memory_validation.h>

#if defined(__SWITCH__) && defined(VITA3K_SWITCH_TRACELAB)
#include <util/switch_validation_schedule.h>
#include <util/switch_write_watch.h>
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
#ifdef VITA3K_SWITCH_WRITE_SHADOW_DEFAULT
    bool write_shadow = true;
#else
    bool write_shadow = false; // Preserve legacy host fixtures and explicit staging mode.
#endif
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
watch::Table live_watch;
thread_local watch::Audit last_hash_audit;
thread_local uint32_t last_hash_address = 0;
thread_local uint64_t last_hash_size = 0;
uint32_t phase_count() { return options.write_shadow ? 4 : 8; }
PhasePosition position_at(uint64_t elapsed, uint64_t settle, uint64_t measure) {
    auto result = phase_position(elapsed, settle, measure);
    if (result.index >= phase_count()) return {phase_count(), true, false, false};
    return result;
}
std::string title;
std::vector<Result> results;
std::mutex results_mutex;
std::atomic<uint64_t> lost_results{0};
uint32_t test_passed = 0, test_failed = 0, test_skipped = 0; // results_mutex
std::mutex status_mutex;
StatusSnapshot published_status; // status_mutex; UI never reads live renderer counters

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
        } else if (key == "validation_experiment") {
            if (value == "write_shadow") out.write_shadow = true;
            else if (value == "staging") out.write_shadow = false;
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
    if (options.write_shadow) live_watch.phase(0, false, true);
}

// Render thread only. Fixed-size copy, no string formatting or disk I/O.
void publish_status(uint64_t tick) {
    StatusSnapshot out;
    const uint64_t hz = armGetSystemTickFreq();
    const uint64_t settle = uint64_t(options.settle_seconds) * hz;
    const uint64_t measure = uint64_t(options.measure_seconds) * hz;
    const uint64_t duration = settle + measure;
    const uint64_t total = duration * phase_count();
    out.phase_count = phase_count();
    out.write_shadow = options.write_shadow;
    const auto observation = live_watch.snapshot();
    out.watched_pages = observation.backing_pages;
    out.watch_failed = observation.failed;
    for (const auto &p : observation.phases) {
        out.first_write_pages += p.first_write_pages;
        out.unobserved_changes += p.changed_unobserved;
        out.exact_write_hits += p.exact_resource_hits;
        out.page_only_writes += p.page_only_stores;
        out.host_write_hits += p.host_resource_hits;
        out.content_changes += p.changed;
        out.guard_budget_exhausted |= p.guard_budget_exhausted != 0;
        out.watch_failed |= p.invalid_accesses != 0;
    }
    out.cache_changes = observation.cache.changes;
    out.automatic = options.automatic;
    out.run_id = run_id;
    out.source_tick = tick;
    out.timebase_hz = hz;
    out.total_ms = uint64_t(options.settle_seconds + options.measure_seconds) * phase_count() * 1000;
    out.intervals_dropped = overflow;
    for (const auto &phase : phases) {
        out.samples += phase.intervals;
        out.publish_calls += phase.publish_calls;
        out.dedup_calls += phase.dedup_calls;
    }
    if (completed) {
        out.stage = Stage::Complete;
        out.phase = phase_count();
        out.progress_per_mille = 1000;
        for (size_t i = 0; i < phase_count(); ++i) {
            out.insufficient_samples |= !phases[i].complete || phases[i].intervals < 100
                || phases[i].ticks < measure * 9 / 10;
            out.missing_treatment |= staging_dedup_order[i] && (options.write_shadow
                ? (!observation.bound || observation.phases[i].armed_pages == 0 || observation.phases[i].covered_checks == 0)
                : (phases[i].dedup_calls == 0 || phases[i].dedup_bytes == 0));
        }
        out.outcome = overflow || out.insufficient_samples || (options.write_shadow && (out.watch_failed || out.guard_budget_exhausted)) ? CaptureOutcome::Invalid
            : out.missing_treatment ? CaptureOutcome::Inconclusive : CaptureOutcome::Ready;
    } else if (aborted) {
        out.stage = Stage::Interrupted;
        out.outcome = CaptureOutcome::Invalid;
    } else if (!run_start_tick) {
        const uint64_t elapsed = first_render_tick && tick >= first_render_tick ? tick - first_render_tick : 0;
        const uint64_t warmup = uint64_t(options.warmup_seconds) * hz;
        const uint64_t left = elapsed < warmup ? warmup - elapsed : 0;
        out.stage = options.automatic ? Stage::Warmup : Stage::Waiting;
        out.stage_remaining_ms = hz ? left * 1000 / hz : 0;
        out.remaining_ms = out.total_ms + (options.automatic ? out.stage_remaining_ms : 0);
    } else {
        const uint64_t elapsed = tick >= run_start_tick ? tick - run_start_tick : 0;
        const auto position = position_at(elapsed, settle, measure);
        out.phase = position.index + 1;
        out.deduplicate = position.deduplicate;
        const uint64_t actual_elapsed = tick >= phase_entry_tick ? tick - phase_entry_tick : 0;
        const bool actually_settled = actual_elapsed >= settle;
        out.stage = position.measuring && actually_settled ? Stage::Measuring : Stage::Settling;
        const uint64_t phase_elapsed = elapsed % duration;
        const uint64_t left = out.stage == Stage::Measuring ? duration - phase_elapsed
            : std::max(phase_elapsed < settle ? settle - phase_elapsed : 0,
                actual_elapsed < settle ? settle - actual_elapsed : 0);
        out.stage_remaining_ms = hz ? left * 1000 / hz : 0;
        out.remaining_ms = hz && elapsed < total ? (total - elapsed) * 1000 / hz : 0;
        out.progress_per_mille = total ? static_cast<uint32_t>(std::min<uint64_t>(999, elapsed * 1000 / total)) : 0;
        if (position.index < phases.size()) out.phase_samples = phases[position.index].intervals;
    }
    const std::lock_guard<std::mutex> lock(status_mutex);
    published_status = out;
}

struct PublishOnReturn {
    uint64_t tick;
    ~PublishOnReturn() { publish_status(tick); }
};

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
    live_watch.reset();
#if defined(__aarch64__)
    live_watch.set_clock(+[]() -> uint64_t { return armGetSystemTick(); });
#endif
    options = read_options();
    title = title_id ? title_id : "unknown";
    {
        const std::lock_guard<std::mutex> lock(results_mutex);
        results.clear();
        test_passed = test_failed = test_skipped = 0;
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
    {
        const std::lock_guard<std::mutex> lock(status_mutex);
        published_status = {};
        published_status.stage = options.enabled ? Stage::Startup : Stage::Disabled;
        published_status.automatic = options.automatic;
    }
    is_enabled.store(options.enabled, std::memory_order_release);
    if (options.enabled)
        LOG_INFO("[memory-validation] isolated tests enabled; strict Hash; capture {}s + {}s settle; L+R+Y starts/restarts; auto={} after {}s",
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
    live_watch.phase(watch::no_index, false);
}

bool enabled() { return is_enabled.load(std::memory_order_relaxed); }
bool strict_hash() { return enabled(); }

StatusSnapshot status_snapshot() {
    if (!enabled()) return {};
    // There is no path that holds results_mutex then status_mutex together.
    // scoped_lock also keeps test counts and the published copy consistent.
    const std::scoped_lock lock(status_mutex, results_mutex);
    StatusSnapshot out = published_status;
    out.passed = test_passed; out.failed = test_failed; out.skipped = test_skipped;
    out.results_dropped = lost_results.load(std::memory_order_relaxed);
    out.restart_pending = start_requested.load(std::memory_order_acquire);
    if (out.stage != Stage::Complete) {
        const bool paused = is_paused.load(std::memory_order_acquire);
        if (paused || abort_requested.load(std::memory_order_acquire)) {
            out.stage = out.run_id ? Stage::Interrupted : Stage::Paused;
            if (out.run_id) out.outcome = CaptureOutcome::Invalid;
        }
    } else if (out.failed || out.results_dropped) {
        out.outcome = CaptureOutcome::Invalid;
    }
    return out;
}

bool deduplicate_staging_cache() { return !options.write_shadow && dedup_enabled.load(std::memory_order_relaxed); }

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
        const auto &saved = results.back().status;
        if (saved == "pass") ++test_passed;
        else if (saved == "skip") ++test_skipped;
        else ++test_failed;
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
    const PublishOnReturn publish{tick};
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
        if (options.write_shadow) live_watch.phase(watch::no_index, false);
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
    const auto position = position_at(current, settle, measure);
    if (!position.complete && measured_interval(previous, current, settle, measure)
        && previous_tick >= phase_entry_tick && previous_tick - phase_entry_tick >= settle) {
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
    if (position_at(previous, settle, measure).index != position.index) {
        if (options.write_shadow) live_watch.phase(position.complete ? watch::no_index : position.index, position.deduplicate, false, true);
        phase_entry_tick = tick;
#if defined(__aarch64__)
        if (options.write_shadow) phase_entry_tick = std::max(tick, uint64_t(armGetSystemTick()));
#endif
    } // Actual policy change, even after a long stall.
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
        LOG_INFO("[memory-validation] phase {}/{}: {} (strict Hash, {}s settle + {}s measurement)",
            state, phase_count(), options.write_shadow
                ? (staging_dedup_order[state - 1] ? "B: persistent range observation" : "A: mirror table, observe off")
                : (staging_dedup_order[state - 1] ? "B: noncoherent staging dedup" : "A: original cache maintenance"),
            options.settle_seconds, options.measure_seconds);
    else if (state == 9)
        LOG_INFO("[memory-validation] completed; normal exit exports all tests and paired phases in the TraceLab folder");
    else if (state == 10)
        LOG_WARN("[memory-validation] run interrupted by pause/focus/menu; L+R+Y starts a fresh paired run");
}

MappingGuard::MappingGuard() {
    if (enabled() && options.write_shadow) lock_ = live_watch.hold();
}

uint8_t **live_write_table(uint8_t **normal) {
    if (!enabled() || !options.write_shadow) return nullptr;
    return live_watch.bind(normal);
}
void live_publish_page(uint8_t **normal, uint32_t page, uint8_t *entry) noexcept {
    if (!enabled() || !options.write_shadow) { normal[page] = entry; return; }
    live_watch.publish(normal, page, entry);
}
void live_detach_table(uint8_t **normal) { live_watch.detach(normal); }
watch::Access live_read(uint8_t **normal, uint32_t address, void *dst, size_t size) {
    if (!enabled() || !options.write_shadow) return watch::Access::Unhandled;
    return live_watch.read(normal, address, dst, size);
}
watch::Access live_write(uint8_t **normal, uint32_t address, const void *src, size_t size) {
    if (!enabled() || !options.write_shadow) return watch::Access::Unhandled;
    return live_watch.write(normal, address, src, size);
}
watch::Access live_exclusive(uint8_t **normal, uint32_t address, uint64_t value,
    uint64_t expected, size_t size, bool &success) {
    if (!enabled() || !options.write_shadow) return watch::Access::Unhandled;
    switch (size) {
    case 1: return live_watch.exclusive<uint8_t>(normal, address, value, expected, success);
    case 2: return live_watch.exclusive<uint16_t>(normal, address, value, expected, success);
    case 4: return live_watch.exclusive<uint32_t>(normal, address, value, expected, success);
    case 8: return live_watch.exclusive<uint64_t>(normal, address, value, expected, success);
    default: success = false; return watch::Access::Unhandled;
    }
}
watch::Token live_hash_begin(uint8_t **normal, uint32_t address, const void *pixels,
    uint64_t size, const void *palette, uint64_t palette_size) {
    last_hash_audit = {};
    last_hash_address = address; last_hash_size = size;
    if (!enabled() || !options.write_shadow) return {};
    return live_watch.before(normal, address, pixels, size, palette, palette_size);
}
void live_hash_end(watch::Token token, uint64_t hash) {
    last_hash_audit = {};
    if (token.resource != watch::no_index) last_hash_audit = live_watch.after(token, hash);
}
void live_host_written(const void *host, uint64_t size, watch::HostWriter writer) {
    if (enabled() && options.write_shadow) live_watch.host_written(host, size, writer);
}
void live_cache_decision(uint32_t slot, uint32_t address, uint64_t size,
    uint64_t previous_hash, uint64_t current_hash, bool ordinary) {
    if (!enabled() || !options.write_shadow) return;
    const auto audit = ordinary && last_hash_address == address && last_hash_size == size
        ? last_hash_audit : watch::Audit{};
    // Ordinary TextureCache hashes are xor-1 tagged; registry hashes are raw.
    live_watch.cache_decision(audit, slot, address, size,
        previous_hash ^ (ordinary ? 1 : 0), current_hash ^ (ordinary ? 1 : 0), ordinary);
    last_hash_audit = {};
}

static void write_shadow_report(FILE *file) {
    const auto snapshot = live_watch.snapshot();
    using U = unsigned long long;
    std::fprintf(file, ",\n  \"write_shadow\": {\n"
        "    \"schema_version\":2,\n    \"bound\":%s,\n    \"failed\":%s,\n"
        "    \"hash_skip_authority\":false,\n    \"scope\":\"JIT_persistent_range_plus_HLE_notices_observational\",\n"
        "    \"arm_safepoint\":false,\n    \"HLE_GPU_coverage\":false,\n"
        "    \"resources\":%llu,\n    \"backing_pages\":%llu,\n    \"late_resources\":%llu,\n"
        "    \"events_dropped\":%llu,\n    \"phases\":[",
        snapshot.bound ? "true" : "false", snapshot.failed ? "true" : "false",
        U(snapshot.resources), U(snapshot.backing_pages), U(snapshot.late_resources), U(snapshot.events_dropped));
    for (uint32_t i = 0; i < phase_count(); ++i) {
        const auto &c = snapshot.phases[i];
        std::fprintf(file, "%s\n      {\"index\":%u,\"hash_checks\":%llu,\"hash_bytes\":%llu,\"covered_checks\":%llu,"
            "\"changed\":%llu,\"changed_cpu_seen\":%llu,\"changed_unobserved\":%llu,\"changed_uncovered\":%llu,"
            "\"callback_stores\":%llu,\"callback_reads\":%llu,\"exclusive_success\":%llu,\"invalid_accesses\":%llu,"
            "\"first_write_pages\":%llu,\"armed_pages\":%llu,\"armed_aliases\":%llu,"
            "\"mapping_updates\":%llu,\"resources_rejected\":%llu",
            i ? "," : "", i, U(c.hash_checks), U(c.hash_bytes), U(c.covered_checks), U(c.changed),
            U(c.changed_cpu_seen), U(c.changed_unobserved), U(c.changed_uncovered),
            U(c.callback_stores), U(c.callback_reads), U(c.exclusive_success), U(c.invalid_accesses),
            U(c.first_write_pages), U(c.armed_pages), U(c.armed_aliases), U(c.mapping_updates), U(c.resources_rejected));
        std::fprintf(file, ",\"watched_stores\":%llu,\"exact_resource_hits\":%llu,\"page_only_stores\":%llu,"
            "\"host_notices\":%llu,\"host_resource_hits\":%llu,\"changed_host_seen\":%llu,"
            "\"changed_raced\":%llu,\"discarded_tokens\":%llu,\"guard_budget_exhausted\":%llu}",
            U(c.watched_stores), U(c.exact_resource_hits), U(c.page_only_stores), U(c.host_notices),
            U(c.host_resource_hits), U(c.changed_host_seen), U(c.changed_raced), U(c.discarded_tokens),
            U(c.guard_budget_exhausted));
    }
    std::fputs("\n    ],\n    \"events\":[", file);
    for (uint64_t i = 0; i < snapshot.event_count; ++i) {
        const auto &e = snapshot.events[i];
        const char *kind = e.kind == watch::Event::FirstWrite ? "first_cpu_write"
            : e.kind == watch::Event::ChangedUnobserved ? "hash_changed_without_observed_CPU_write"
            : e.kind == watch::Event::MappingChange ? "mapping_change" : "invalid_callback_access";
        std::fprintf(file, "%s\n      {\"kind\":\"%s\",\"phase\":%u,\"address\":%u,\"size\":%u,\"resource\":%u,"
            "\"tick\":%llu,\"epoch\":%llu,\"before_hash_hex\":\"%016llx\",\"after_hash_hex\":\"%016llx\"}",
            i ? "," : "", kind, e.phase, e.address, e.size, e.resource, U(e.tick), U(e.epoch), U(e.before_hash), U(e.after_hash));
    }
    std::fputs("\n    ],\n    \"footprints\":[", file);
    const auto footprints = live_watch.footprints();
    for (size_t i = 0; i < footprints.size(); ++i) {
        const auto &r = footprints[i];
        std::fprintf(file, "%s\n      {\"id\":%u,\"guest_address\":%u,\"pixel_host_hex\":\"%016llx\",\"size\":%llu,"
            "\"palette_host_hex\":\"%016llx\",\"palette_size\":%llu}", i ? "," : "", r.id, r.address,
            U(r.key.pixels), U(r.key.size), U(r.key.palette), U(r.key.palette_size));
    }
    std::fprintf(file, "\n    ],\n    \"capacity_limited\":%s,\n"
        "    \"persistent_selected_pages\":true,\n    \"cache_ledger\":{\n"
        "      \"scope\":\"session_including_warmup_and_restarts\",\n"
        "      \"comparisons\":%llu,\n      \"changes\":%llu,\n      \"events_dropped\":%llu,\n      \"reasons\":{",
        snapshot.capacity_limited ? "true" : "false", U(snapshot.cache.comparisons), U(snapshot.cache.changes), U(snapshot.cache.dropped));
    for (size_t i = 0; i < snapshot.cache.reasons.size(); ++i)
        std::fprintf(file, "%s\"%s\":%llu", i ? "," : "", watch::cache_reason_name(static_cast<watch::CacheReason>(i)), U(snapshot.cache.reasons[i]));
    std::fputs("},\n      \"events\":[", file);
    for (uint64_t i = 0; i < snapshot.cache.stored; ++i) {
        const auto &e = snapshot.cache.events[i];
        std::fprintf(file, "%s\n        {\"phase\":%u,\"slot\":%u,\"address\":%u,\"size\":%llu,\"resource\":%u,"
            "\"tick\":%llu,\"epoch\":%llu,\"reason\":\"%s\",\"previous_hash_hex\":\"%016llx\","
            "\"current_hash_hex\":\"%016llx\",\"observer_previous_hash_hex\":\"%016llx\"}",
            i ? "," : "", e.phase, e.slot, e.address, U(e.size), e.resource, U(e.tick), U(e.epoch),
            watch::cache_reason_name(e.reason), U(e.previous_hash), U(e.current_hash), U(e.observer_previous_hash));
    }
    std::fputs("\n      ]\n    }\n  }", file);
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
        "\n  ],\n  \"benchmark\": {\n    \"order\":\"%s\",\n    \"experiment\":\"%s\",\n"
        "    \"auto_start\":%s,\n    \"warmup_seconds\":%u,\n    \"settle_seconds\":%u,\n    \"measure_seconds\":%u,\n"
        "    \"requested\":%s,\n    \"completed\":%s,\n    \"aborted\":%s,\n    \"run_id\":%u,\n"
        "    \"frames_overflow\":%llu,\n    \"phases\":[",
        options.write_shadow ? "ABBA" : "ABBA/BAAB", options.write_shadow ? "live_write_shadow" : "staging_dedup",
        options.automatic ? "true" : "false", options.warmup_seconds, options.settle_seconds, options.measure_seconds,
        requested ? "true" : "false", completed ? "true" : "false", aborted ? "true" : "false", run_id,
        static_cast<unsigned long long>(overflow));
    for (size_t i = 0; i < phase_count(); ++i) {
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
            i ? "," : "", i, staging_dedup_order[i] ? (options.write_shadow ? "write_watch" : "dedup") : "baseline", p.complete ? "true" : "false",
            static_cast<unsigned long long>(p.intervals), duration_ms,
            p.ticks ? double(p.intervals) * hz / p.ticks : 0,
            percentile(milliseconds, 0.50), percentile(milliseconds, 0.95), percentile(milliseconds, 0.99),
            static_cast<unsigned long long>(p.publish_calls), static_cast<unsigned long long>(p.publish_bytes),
            static_cast<unsigned long long>(p.dedup_calls), static_cast<unsigned long long>(p.dedup_bytes),
            static_cast<unsigned long long>(p.excluded_intervals), 1000.0 * p.excluded_ticks / hz);
    }
    const auto summary = status_snapshot();
    std::fprintf(file, "\n    ]\n  },\n  \"validation_revision\":4,\n"
        "  \"capture_outcome\":\"%s\",\n  \"overlay_refresh_hz\":4,\n"
        "  \"overlay_completion_is_not_graphics_validation\":true", outcome_name(summary.outcome));
    if (options.write_shadow) write_shadow_report(file);
    std::fputs("\n}\n", file);
    const bool write_error = std::ferror(file) != 0;
    const bool close_error = std::fclose(handle.release()) != 0;
    if (write_error || close_error)
        LOG_ERROR("[memory-validation] incomplete report {}", path);
    else
        LOG_INFO("[memory-validation] wrote {}", path);
}
} // namespace switch_validation
#endif
