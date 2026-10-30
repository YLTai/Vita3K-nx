// Retrospective evidence for an entry which was pending at an earlier Hash.
// Full reference Hash is still returned. This is NOT a live reuse authority.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <util/hle_range_gate.h>

namespace switch_validation::epoch {
enum class ResolutionBlock : unsigned { Accepted, Invalid, OutputBusy, Pending, Version, History };

inline ResolutionBlock resolve_declarations(const RangeStamp &a, const RangeStamp &b,
    const RangeStamp &c, const RangeStamp &d) noexcept {
    const RangeStamp *points[]{&a, &b, &c, &d};
    for (const auto *s : points) if (!s->valid) return ResolutionBlock::Invalid;
    // Explicit output intervals are never retrospectively waived.
    for (const auto *s : points) if (s->busy) return ResolutionBlock::OutputBusy;
    // A pending import may resolve ONLY before the final decision. Unknown
    // finishes/parks/prefixes advance unknown; overlapping declarations advance
    // version before writing. Both histories must survive unchanged across ALL
    // four endpoints, including the beginning of the previous full Hash.
    if (d.unknown_busy) return ResolutionBlock::Pending;
    for (const auto *s : points) if (s->version != d.version) return ResolutionBlock::Version;
    for (const auto *s : points) if (s->unknown != d.unknown) return ResolutionBlock::History;
    return ResolutionBlock::Accepted;
}

struct ResolutionCounts {
    uint64_t checked = 0, checked_bytes = 0, strict = 0, strict_bytes = 0;
    uint64_t resolved = 0, resolved_bytes = 0, additional = 0, additional_bytes = 0;
    uint64_t additional_start_idle = 0, additional_start_idle_bytes = 0;
    uint64_t additional_during_hash = 0, additional_during_hash_bytes = 0;
    uint64_t submission = 0, submission_bytes = 0, gpu_history = 0, gpu_history_bytes = 0;
    uint64_t invalid = 0, output_busy = 0, pending = 0, version = 0, history = 0;
    uint64_t mismatches = 0, additional_mismatches = 0;
};
struct ResolutionEvent {
    unsigned phase = 0, resource = 0;
    uint32_t address = 0;
    uint64_t tick = 0, ticket = 0, bytes = 0, previous = 0, current = 0;
    std::array<uint64_t, 4> pending{};
    bool additional = false;
};
struct ResolutionAudit {
    std::array<ResolutionCounts, 4> phases{};
    std::array<ResolutionEvent, 32> events{};
    uint64_t stored = 0, dropped = 0;
    bool failed = false;

    void observe(unsigned phase, unsigned resource, uint32_t address, uint64_t bytes,
        uint64_t ticket, uint64_t previous, uint64_t current, bool strict,
        const RangeStamp &a, const RangeStamp &b, const RangeStamp &c, const RangeStamp &d,
        uint64_t (*clock)() = nullptr) noexcept {
        if (phase != 1 && phase != 2) { failed = true; return; }
        auto &n = phases[phase];
        const auto add = [&](uint64_t &value, uint64_t delta = 1) {
            if (delta > UINT64_MAX - value) failed = true;
            else value += delta;
        };
        add(n.checked); add(n.checked_bytes, bytes);
        if (strict) { add(n.strict); add(n.strict_bytes, bytes); }
        const auto reason = resolve_declarations(a, b, c, d);
        if (strict && reason != ResolutionBlock::Accepted) failed = true;
        switch (reason) {
        case ResolutionBlock::Invalid: add(n.invalid); return;
        case ResolutionBlock::OutputBusy: add(n.output_busy); return;
        case ResolutionBlock::Pending: add(n.pending); return;
        case ResolutionBlock::Version: add(n.version); return;
        case ResolutionBlock::History: add(n.history); return;
        default: break;
        }
        add(n.resolved); add(n.resolved_bytes, bytes);
        if (!strict) {
            add(n.additional); add(n.additional_bytes, bytes);
            // Resolution happening during the reference Hash cannot be priced
            // as work avoidable before that Hash. Export the two cases apart.
            if (c.unknown_busy) { add(n.additional_during_hash); add(n.additional_during_hash_bytes, bytes); }
            else { add(n.additional_start_idle); add(n.additional_start_idle_bytes, bytes); }
        }
        if (a.gpu == d.gpu && b.gpu == d.gpu && c.gpu == d.gpu) {
            add(n.submission); add(n.submission_bytes, bytes);
        } else { add(n.gpu_history); add(n.gpu_history_bytes, bytes); }
        if (previous == current) return;
        add(n.mismatches);
        if (!strict) add(n.additional_mismatches);
        if (stored < events.size()) events[stored++] = {phase, resource, address,
            clock ? clock() : 0, ticket, bytes, previous, current,
            {a.unknown_busy, b.unknown_busy, c.unknown_busy, d.unknown_busy}, !strict};
        else add(dropped);
    }
    void write_json(FILE *f) const {
        using U = unsigned long long;
        std::fprintf(f, "{\"version\":1,\"authority\":false,\"full_hash_each_check\":true,"
            "\"GPU_completion_covered\":false,\"async_HLE_covered\":false,\"failed\":%s,"
            "\"rule\":\"four_endpoints_final_unknown_idle\","
            "\"event_phase_model\":\"including_settling\",\"phases\":[", failed ? "true" : "false");
        for (unsigned i = 0; i < phases.size(); ++i) {
            const auto &n = phases[i];
            std::fprintf(f, "%s{\"index\":%u,\"checked\":%llu,\"checked_bytes\":%llu,"
                "\"strict\":%llu,\"strict_bytes\":%llu,\"resolved\":%llu,\"resolved_bytes\":%llu,"
                "\"additional\":%llu,\"additional_bytes\":%llu,\"additional_start_idle\":%llu,\"additional_start_idle_bytes\":%llu,"
                "\"additional_during_hash\":%llu,\"additional_during_hash_bytes\":%llu,\"submission\":%llu,\"submission_bytes\":%llu,"
                "\"gpu_history\":%llu,\"gpu_history_bytes\":%llu,\"invalid\":%llu,\"output_busy\":%llu,\"pending\":%llu,"
                "\"version\":%llu,\"history\":%llu,\"mismatches\":%llu,\"additional_mismatches\":%llu}",
                i ? "," : "", i, U(n.checked), U(n.checked_bytes), U(n.strict), U(n.strict_bytes),
                U(n.resolved), U(n.resolved_bytes), U(n.additional), U(n.additional_bytes),
                U(n.additional_start_idle), U(n.additional_start_idle_bytes), U(n.additional_during_hash), U(n.additional_during_hash_bytes),
                U(n.submission), U(n.submission_bytes), U(n.gpu_history), U(n.gpu_history_bytes), U(n.invalid), U(n.output_busy),
                U(n.pending), U(n.version), U(n.history), U(n.mismatches), U(n.additional_mismatches));
        }
        std::fprintf(f, "],\"events_dropped\":%llu,\"events\":[", U(dropped));
        for (uint64_t i = 0; i < stored; ++i) {
            const auto &e = events[i];
            std::fprintf(f, "%s{\"phase\":%u,\"resource\":%u,\"address\":%u,\"tick\":%llu,"
                "\"publication\":%llu,\"bytes\":%llu,\"previous_hash_hex\":\"%016llx\","
                "\"current_hash_hex\":\"%016llx\",\"pending\":[%llu,%llu,%llu,%llu],\"additional\":%s}",
                i ? "," : "", e.phase, e.resource, e.address, U(e.tick), U(e.ticket), U(e.bytes),
                U(e.previous), U(e.current), U(e.pending[0]), U(e.pending[1]), U(e.pending[2]),
                U(e.pending[3]), e.additional ? "true" : "false");
        }
        std::fputs("]}", f);
    }
};

// Runs after the existing Table::after has committed its ordinary full-Hash
// observation. Reuses its exact endpoint stamp: no extra domain lock, wait,
// callback, hash, allocation, or ordinary Guest-store instrumentation.
// The guard is constructed AFTER the mapping/token rejection, under Table lock.
template<class Resource, class Counts, class Token, class Audit>
class ResolutionScope {
    Resource &r_;
    Counts &counts_;
    ResolutionAudit &out_;
    const Token &token_;
    const Audit &audit_;
    RangeStamp previous_begin_, previous_end_;
    uint64_t cpu_, strict_;
    unsigned phase_;
    uint64_t (*clock_)();
public:
    ResolutionScope(Resource &r, Counts &counts, ResolutionAudit &out,
        const Token &token, const Audit &audit, unsigned phase, uint64_t (*clock)()) noexcept
        : r_(r), counts_(counts), out_(out), token_(token), audit_(audit),
          previous_begin_(r.resolution_begin), previous_end_(r.baseline_publication.ranges),
          cpu_(counts.cpu_candidates), strict_(counts.hle_candidates), phase_(phase), clock_(clock) {}
    ~ResolutionScope() noexcept {
        if (counts_.cpu_candidates != cpu_) {
            if (counts_.cpu_candidates != cpu_ + 1 || counts_.hle_candidates < strict_
                || counts_.hle_candidates - strict_ > 1) out_.failed = true;
            out_.observe(phase_, token_.resource, r_.address, r_.key.size + r_.key.palette_size,
                token_.publication.ticket, audit_.previous_hash, audit_.current_hash,
                counts_.hle_candidates != strict_, previous_begin_, previous_end_,
                token_.publication.ranges, r_.baseline_publication.ranges, clock_);
        }
        r_.resolution_begin = token_.publication.ranges;
    }
    ResolutionScope(const ResolutionScope &) = delete;
    ResolutionScope &operator=(const ResolutionScope &) = delete;
};
} // namespace switch_validation::epoch
