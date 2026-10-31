// Bridge current observer stamps to the separately authorized reuse core.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <util/resource_hash_reuse.h>
#include <util/resource_write_ledger.h>

namespace switch_validation::reuse {
// Coverage MUST originate in a registered producer/ownership contract. This
// function never upgrades legacy strict/resolved candidate counters to proof.
// Token is the actual live Table::before result. fully_watched() rechecks its
// lifetime, current version and persistent selected-page coverage under lock.
// The real asynchronous/GPU producer hooks are still prerequisites for granting
// their bits; an empty ledger or unchanged submission number is NOT coverage.
template<class Table, class Token>
Evidence observer_evidence(Table &table, const Token &t, const WriteStamp &external,
    uint32_t producer_coverage) noexcept {
    Evidence e;
    if (t.resource == UINT32_MAX || !t.epoch || t.mapping_version == UINT64_MAX
        || !external.valid || !external.lifetime || !table.fully_watched(t)) return e;
    const auto &p = t.publication;
    e.allocation = uint64_t(t.resource) + 1;
    e.lifetime = t.epoch;
    e.mapping = t.mapping_version + 1;
    e.publication = p.ticket;
    e.cpu = t.cpu_version;
    e.host = p.ranges.version;
    e.host_notice = t.host_version;
    e.external_host = external.host;
    e.external_lifetime = external.lifetime;
    e.async = external.async;
    e.gpu = external.gpu;
    e.unknown = p.ranges.unknown;
    e.coverage = producer_coverage;
    e.valid = p.valid && p.ranges.valid;
    e.publication_drained = p.drained;
    e.host_busy = p.ranges.busy || external.host_busy;
    e.unknown_busy = p.ranges.unknown_busy;
    e.async_busy = external.async_busy;
    e.gpu_busy = external.gpu_busy;
    // This bit is meaningful only for sources whose GPU completion/visibility
    // path was connected before their registration (the GpuWrites contract).
    e.gpu_host_visible = !external.gpu_busy && (producer_coverage & GpuWrites);
    return e;
}
} // namespace switch_validation::reuse
