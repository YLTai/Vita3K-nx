// Evidence for the reviewed LwCondCB polling prefix, NOT content-reuse authority.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace switch_validation::epoch {
class CallbackPollAudit {
public:
    // Events are assigned to the phase in which they complete/are consumed.
    struct Counts {
        uint64_t polls = 0, empty = 0, nonempty = 0, incomplete = 0;
        uint64_t reported_callbacks = 0, uses = 0, clean_uses = 0, conservative_uses = 0, abandoned = 0;
    };
private:
    mutable std::mutex mutex_;
    Counts total_;
    std::array<Counts, 4> phases_{};
    unsigned phase_ = 4;
    uint64_t capture_serial_ = 0;
    std::atomic<bool> failed_{false};
    void add(uint64_t Counts::*field, uint64_t n = 1) {
        auto increment = [&](Counts &c) {
            if (n > UINT64_MAX - c.*field) failed_.store(true, std::memory_order_relaxed);
            else c.*field += n;
        };
        increment(total_);
        if (phase_ < phases_.size()) increment(phases_[phase_]);
    }
    static void write_counts(FILE *f, const Counts &c) {
        using U = unsigned long long;
        std::fprintf(f, "{\"polls\":%llu,\"empty\":%llu,\"nonempty\":%llu,\"incomplete\":%llu,"
            "\"reported_callbacks\":%llu,\"uses\":%llu,\"clean_uses\":%llu,\"conservative_uses\":%llu,\"abandoned\":%llu}",
            U(c.polls), U(c.empty), U(c.nonempty), U(c.incomplete), U(c.reported_callbacks),
            U(c.uses), U(c.clean_uses), U(c.conservative_uses), U(c.abandoned));
    }
public:
    bool failed() const { return failed_.load(std::memory_order_relaxed); }
    void reset() {
        std::lock_guard lock(mutex_); total_ = {}; phases_ = {}; phase_ = 4; capture_serial_ = 0; failed_ = false;
    }
    void phase(unsigned p, bool restart) {
        std::lock_guard lock(mutex_);
        if (restart) {
            phases_ = {};
            if (capture_serial_ == UINT64_MAX) failed_ = true;
            else ++capture_serial_;
        }
        phase_ = p < 4 ? p : 4;
    }
    void poll(bool complete, uint32_t callbacks) {
        std::lock_guard lock(mutex_); add(&Counts::polls);
        add(!complete ? &Counts::incomplete : callbacks ? &Counts::nonempty : &Counts::empty);
        add(&Counts::reported_callbacks, callbacks);
    }
    bool use(bool clean) {
        std::lock_guard lock(mutex_); clean &= !failed(); add(&Counts::uses);
        add(clean ? &Counts::clean_uses : &Counts::conservative_uses); return clean && !failed();
    }
    void abandon() { std::lock_guard lock(mutex_); add(&Counts::abandoned); failed_ = true; }
    Counts snapshot() const { std::lock_guard lock(mutex_); return total_; }
    void write_json(FILE *f) const {
        std::lock_guard lock(mutex_);
        std::fprintf(f, "{\"version\":1,\"authority\":false,\"failed\":%s,"
            "\"event_phase_model\":\"event_time_including_settling\",\"capture_serial\":%llu,\"active_phase\":%u,\"total\":",
            failed() ? "true" : "false", static_cast<unsigned long long>(capture_serial_), phase_);
        write_counts(f, total_); std::fputs(",\"phases\":[", f);
        for (unsigned i = 0; i < phases_.size(); ++i) { if (i) std::fputc(',', f); write_counts(f, phases_[i]); }
        std::fputs("]}", f);
    }
};
struct CallbackPrefixContext {
    const void *owner = nullptr;
    CallbackPollAudit *audit = nullptr;
    bool *tainted = nullptr; // thread-local root lease lifetime
};
class CallbackPrefixScope {
    CallbackPrefixContext context_;
    CallbackPrefixScope *previous_;
    bool finished_ = false, complete_empty_ = false, consumed_ = false;
    inline static thread_local CallbackPrefixScope *current_ = nullptr;
public:
    explicit CallbackPrefixScope(CallbackPrefixContext context) : context_(context), previous_(current_) { current_ = this; }
    ~CallbackPrefixScope() {
        if (!finished_ && context_.owner) {
            if (context_.tainted) *context_.tainted = true;
            if (context_.audit) context_.audit->abandon();
        }
        current_ = previous_;
    }
    CallbackPrefixScope(const CallbackPrefixScope &) = delete;
    CallbackPrefixScope &operator=(const CallbackPrefixScope &) = delete;
    void finish(bool full_scan, uint32_t reported_callbacks) {
        if (finished_) { complete_empty_ = false; if (context_.tainted) *context_.tainted = true; return; }
        finished_ = true; complete_empty_ = full_scan && reported_callbacks == 0;
        if (!complete_empty_ && context_.tainted) *context_.tainted = true;
        if (context_.audit) context_.audit->poll(full_scan, reported_callbacks);
    }
    // Only the reviewed wrapper and its direct forwarding export may consume
    // this proof. An arbitrary caller, an early return, recursion suppression,
    // a missing hook or ANY reported callback retains the old unknown prefix.
    static bool prior_unknown(CallbackPrefixContext context, const char *name) {
        if (!context.owner || !name || (std::strcmp(name, "sceKernelWaitLwCondCB") && std::strcmp(name, "_sceKernelWaitLwCondCB"))) return true;
        auto *p = current_;
        const bool matching = p && p->context_.owner == context.owner && p->finished_ && !p->consumed_;
        const bool clean = matching && p->complete_empty_ && context.tainted && !*context.tainted;
        if (matching) p->consumed_ = true;
        if (!clean && context.tainted) *context.tainted = true;
        return !(context.audit && context.audit->use(clean));
    }
};
} // namespace switch_validation::epoch
