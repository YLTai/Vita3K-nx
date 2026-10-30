// Range-scoped synchronous HLE evidence. Caller holds the execution-domain mutex.
// Unknown imports always remain conservative. This is NOT Hash-reuse authority.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <unordered_map>
#include <vector>

namespace switch_validation::epoch {
struct HostSpan {
    uintptr_t begin = 0;
    uint64_t size = 0;
    HostSpan() = default;
    HostSpan(const void *p, uint64_t n) : begin(reinterpret_cast<uintptr_t>(p)), size(p ? n : 0) {}
    bool operator==(const HostSpan &) const = default;
    bool valid() const { return !size || (begin && size <= UINTPTR_MAX - begin); }
    bool overlaps(const HostSpan &s) const {
        return size && s.size && (begin <= s.begin ? s.begin - begin < size : begin - s.begin < s.size);
    }
};
struct RangeStamp {
    uint64_t version = 0, unknown = 0, gpu = 0, busy = 0, unknown_busy = 0;
    bool valid = false;
};
// Bounded active imports and texture interests. Small output spans use a page
// index; no scan of all textures on every mutex/unlock or tiny libc output.
class HleRanges {
public:
    static constexpr unsigned no_slot = ~0u, max_imports = 128, max_resources = 1024;
    struct Row {
        uint32_t nid = 0;
        const char *name = nullptr; // make_bridge supplies immutable static names
        uint64_t calls = 0, scoped = 0, unknown_samples = 0, overlap_samples = 0;
        uint64_t parks = 0, resumes = 0, parked_samples = 0;
    };
private:
    struct Import { bool used = false, scoped = false, parked = false; uint32_t row = no_slot;
        const char *name = nullptr; std::array<HostSpan, 3> spans{}; };
    struct Interest { bool used = false; std::array<HostSpan, 2> spans{};
        uint64_t version = 0, busy = 0, visited = 0; };
    std::array<Import, max_imports> imports_{};
    std::array<Interest, max_resources> resources_{};
    std::array<Row, 256> rows_{};
    // Attribution acceleration only. Every hit verifies the full NID; a direct
    // cache collision falls back to the original bounded table.
    std::array<unsigned, 1024> row_cache_{};
    struct NoOverlap {
        uint64_t revision = 0;
        std::array<HostSpan, 3> spans{};
    };
    std::array<NoOverlap, 256> no_overlap_{};
    uint64_t interests_revision_ = 1;
    uint64_t row_hits_ = 0, row_probes_ = 0, range_hits_ = 0, range_scans_ = 0;
    std::unordered_map<uintptr_t, std::vector<unsigned>> pages_;
    uint64_t serial_ = 0, unknown_ = 0, gpu_ = 0, unknown_busy_ = 0;
    uint64_t calls_ = 0, exits_ = 0, scoped_ = 0, samples_ = 0, missing_names_ = 0;
    size_t edges_ = 0;
    uint64_t parks_ = 0, resumes_ = 0, parked_ = 0;
    bool failed_ = false;
    uint64_t advance(uint64_t &v) {
        if (v == UINT64_MAX) { failed_ = true; return v; }
        return ++v;
    }
    static bool overlaps(const Interest &r, const Import &i) {
        for (const auto &a : r.spans) for (const auto &b : i.spans) if (a.overlaps(b)) return true;
        return false;
    }
    void touch(Import &i, bool begin) {
        // Cache ONLY a proved empty intersection, never content or validity.
        // watch()/clear_interests() invalidate it even while a lease is live.
        auto *cached = i.row < no_overlap_.size() ? &no_overlap_[i.row] : nullptr;
        if (cached && cached->revision == interests_revision_ && cached->spans == i.spans) {
            ++range_hits_; return;
        }
        ++range_scans_;
        bool hit = false;
        const auto serial = advance(serial_);
        const auto visit = [&](unsigned id) {
            auto &r = resources_[id];
            if (!r.used || r.visited == serial || !overlaps(r, i)) return;
            hit = true;
            r.visited = serial; r.version = serial;
            if (begin) ++r.busy;
            else if (r.busy) --r.busy;
            else failed_ = true;
        };
        for (const auto &s : i.spans) if (s.size && !pages_.empty()) {
            const auto first = s.begin >> 12, last = (s.begin + s.size - 1) >> 12;
            if (last - first > pages_.size()) {
                for (unsigned id = 0; id < max_resources; ++id) visit(id);
            } else for (auto p = first;; ++p) {
                if (auto it = pages_.find(p); it != pages_.end()) for (auto id : it->second) visit(id);
                if (p == last) break;
            }
        }
        if (cached && !hit && !failed_) *cached = {interests_revision_, i.spans};
    }
public:
    HleRanges() { row_cache_.fill(no_slot); }
    bool reset() {
        for (const auto &i : imports_) if (i.used) { failed_ = true; return false; }
        imports_ = {}; resources_ = {}; rows_ = {}; pages_.clear(); edges_ = 0;
        row_cache_.fill(no_slot); no_overlap_ = {}; interests_revision_ = 1;
        row_hits_ = row_probes_ = range_hits_ = range_scans_ = 0;
        serial_ = unknown_ = gpu_ = unknown_busy_ = calls_ = exits_ = scoped_ = samples_ = missing_names_ = 0;
        parks_ = resumes_ = parked_ = 0;
        failed_ = false; return true;
    }
    void clear_interests() { resources_ = {}; pages_.clear(); edges_ = 0; advance(serial_); advance(interests_revision_); }
    bool failed() const { return failed_; }
    unsigned enter(uint32_t nid) {
        const unsigned bucket = (uint32_t(nid * 2654435761u) >> 22);
        unsigned row = row_cache_[bucket];
        if (row < rows_.size() && rows_[row].calls && rows_[row].nid == nid) {
            ++row_hits_; ++rows_[row].calls;
        } else {
            row = no_slot;
            for (unsigned n = 0; n < rows_.size(); ++n) {
                ++row_probes_;
                auto &r = rows_[(nid + n) % rows_.size()];
                if (!r.calls || r.nid == nid) { row = unsigned(&r - rows_.data()); r.nid = nid; ++r.calls; break; }
            }
            row_cache_[bucket] = row;
        }
        if (row == no_slot) ++missing_names_; // attribution capacity is not correctness capacity
        ++calls_;
        for (unsigned n = 0; n < imports_.size(); ++n) if (!imports_[n].used) {
            imports_[n] = {}; imports_[n].used = true; imports_[n].row = row; ++unknown_busy_; return n;
        }
        failed_ = true; return no_slot;
    }
    void name(unsigned slot, const char *name) {
        if (slot >= imports_.size() || !imports_[slot].used || !name) return;
        auto &i = imports_[slot];
        // A nested bridge must not silently change the enclosing contract.
        if (i.name && std::strcmp(i.name, name)) return;
        i.name = name;
        if (i.row < rows_.size()) rows_[i.row].name = name;
    }
    bool declare(unsigned slot, std::initializer_list<const char *> names,
        std::initializer_list<HostSpan> spans, bool prior_unknown = false) {
        if (failed_ || slot >= imports_.size()) return false;
        auto &i = imports_[slot];
        if (!i.used || i.scoped || i.parked || !i.name || spans.size() > i.spans.size()) return false;
        bool allowed = false;
        for (auto n : names) if (!std::strcmp(n, i.name)) allowed = true;
        if (!allowed) return false; // helper reached through an unreviewed caller stays unknown
        for (const auto &s : spans) if (!s.valid()) return false;
        std::copy(spans.begin(), spans.end(), i.spans.begin());
        // Callback-capable wrappers may have executed arbitrary code BEFORE
        // the reviewed wait tail. Do not erase that unknown prefix's effects.
        if (prior_unknown) advance(unknown_);
        --unknown_busy_; i.scoped = true; ++scoped_;
        if (i.row < rows_.size()) ++rows_[i.row].scoped;
        touch(i, true); return true;
    }
    // Only an audited native wait may use this. This is not a read-only HLE
    // declaration: arbitrary prefix/suffix writes remain unknown. Scoped output
    // spans stay busy, including writes performed by a signalling thread.
    bool park(unsigned slot, const char *root_name) {
        if (failed_ || slot >= imports_.size()) return false;
        auto &i = imports_[slot];
        if (!i.used) { failed_ = true; return false; }
        if (i.scoped || i.parked) return false;
        name(slot, root_name);
        if (!unknown_busy_) { failed_ = true; return false; }
        --unknown_busy_; i.parked = true; ++parked_; ++parks_;
        advance(unknown_); // Do not erase unknown writes before entering the wait.
        if (i.row < rows_.size()) ++rows_[i.row].parks;
        return true;
    }
    void resume(unsigned slot) {
        if (slot >= imports_.size() || !imports_[slot].used || !imports_[slot].parked || !parked_) {
            failed_ = true; return;
        }
        auto &i = imports_[slot]; i.parked = false;
        --parked_; ++unknown_busy_; ++resumes_; advance(unknown_);
        if (i.row < rows_.size()) ++rows_[i.row].resumes;
    }
    void leave(unsigned slot) {
        if (slot >= imports_.size() || !imports_[slot].used) { failed_ = true; return; }
        auto &i = imports_[slot];
        if (i.parked) { // A wait guard must unwind BEFORE its import.
            failed_ = true; --parked_; ++resumes_; advance(unknown_);
            if (i.row < rows_.size()) ++rows_[i.row].resumes;
        } else if (i.scoped) touch(i, false);
        else if (unknown_busy_) { --unknown_busy_; advance(unknown_); }
        else failed_ = true;
        i = {}; ++exits_;
    }
    void watch(unsigned id, HostSpan pixels, HostSpan palette) noexcept {
        if (failed_ || id >= resources_.size() || !pixels.valid() || !palette.valid()) { failed_ = true; return; }
        try {
            auto &r = resources_[id];
            if (r.used) { failed_ = true; return; } // clear_interests precedes ID reuse
            advance(interests_revision_);
            r.used = true; r.spans = {pixels, palette}; r.version = advance(serial_);
            for (const auto &i : imports_) if (i.used && i.scoped && overlaps(r, i)) ++r.busy;
            for (const auto &s : r.spans) if (s.size) {
                const auto first = s.begin >> 12, last = (s.begin + s.size - 1) >> 12;
                if (last - first >= 131072 || edges_ + last - first + 1 > 131072) { failed_ = true; return; }
                for (auto p = first;; ++p) {
                    auto &v = pages_[p]; if (std::find(v.begin(), v.end(), id) == v.end()) { v.push_back(id); ++edges_; }
                    if (p == last) break;
                }
            }
        } catch (...) { failed_ = true; }
    }
    void gpu_boundary() { advance(gpu_); }
    RangeStamp stamp(unsigned id) const {
        if (id >= resources_.size() || !resources_[id].used) return {};
        const auto &r = resources_[id]; return {r.version, unknown_, gpu_, r.busy, unknown_busy_, !failed_};
    }
    // Called only for actual texture candidate checks, not sampled HLE calls.
    void explain(unsigned id) {
        if (id >= resources_.size() || !resources_[id].used) return;
        ++samples_;
        // Attribution only is sampled; every eligibility stamp remains exact.
        if ((samples_ & 63u) != 1u) return;
        for (const auto &i : imports_) if (i.used && i.row < rows_.size()) {
            if (i.parked) ++rows_[i.row].parked_samples;
            else if (!i.scoped) ++rows_[i.row].unknown_samples;
            else if (overlaps(resources_[id], i)) ++rows_[i.row].overlap_samples;
        }
    }
    void write_json(FILE *f) const {
        using U = unsigned long long;
        std::fprintf(f,"{\"version\":1,\"failed\":%s,\"calls\":%llu,\"exits\":%llu,\"scoped_calls\":%llu,"
            "\"candidate_explanations\":%llu,\"unattributed_calls\":%llu,\"attribution_period\":64,\"authority\":false,\"imports\":[",
            failed_?"true":"false",U(calls_),U(exits_),U(scoped_),U(samples_),U(missing_names_));
        // Version 1 remains readable; wait segmentation is an explicit extension.
        // parks = resumes + active_parked, also after an interrupted capture.
        bool first = true;
        for (const auto &r : rows_) if (r.calls) {
            // Names originate in compiled export identifiers; never Guest input.
            std::fprintf(f,"%s{\"nid\":%u,\"name\":\"%s\",\"calls\":%llu,\"scoped\":%llu,"
                "\"unknown_candidate_checks\":%llu,\"overlap_candidate_checks\":%llu,\"parks\":%llu,\"resumes\":%llu,\"parked_candidate_checks\":%llu}",first?"":",",
                r.nid,r.name?r.name:"unresolved",U(r.calls),U(r.scoped),U(r.unknown_samples),U(r.overlap_samples),U(r.parks),U(r.resumes),U(r.parked_samples)); first = false;
        }
        std::fprintf(f, "],\"metadata_cache\":{\"version\":1,\"nid_hits\":%llu,\"nid_probes\":%llu,"
            "\"no_overlap_hits\":%llu,\"range_scans\":%llu,\"interests_revision\":%llu,\"content_reuse_authority\":false}",
            U(row_hits_),U(row_probes_),U(range_hits_),U(range_scans_),U(interests_revision_));
        std::fprintf(f,",\"native_waits\":{\"version\":1,\"parks\":%llu,\"resumes\":%llu,"
            "\"active\":%llu,\"async_outputs_covered\":false}}",U(parks_),U(resumes_),U(parked_));
    }
};
inline bool stable_host(const RangeStamp &a, const RangeStamp &b) {
    return a.valid && b.valid && !a.busy && !b.busy && !a.unknown_busy && !b.unknown_busy
        && a.version == b.version && a.unknown == b.unknown;
}
} // namespace switch_validation::epoch
