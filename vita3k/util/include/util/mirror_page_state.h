// Explicit CPU/GPU ownership for a private, single-owner test resource.
// No content hash is a write event. Completion is required before publication.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <algorithm>

namespace switch_validation::mirror {
enum class Owner : uint8_t { Cpu, Both, Gpu, Invalid };
template<size_t Pages, size_t PageSize = 4096> class PageState {
    static_assert(Pages && Pages <= 32 && PageSize);
    std::array<Owner, Pages> owner_{};
    bool valid_ = true;
public:
    static constexpr size_t bytes = Pages * PageSize;
    uint64_t uploaded = 0, downloaded = 0, cpu_events = 0, gpu_events = 0, clean_uses = 0;
    PageState() { owner_.fill(Owner::Cpu); }
    bool valid() const { return valid_; }
    bool range(size_t offset, size_t size) const { return offset <= bytes && size <= bytes - offset; }
    Owner owner(size_t page) const { return page < Pages ? owner_[page] : Owner::Invalid; }
    void fail() { valid_ = false; owner_.fill(Owner::Invalid); }
    template<class Transfer> bool sync(size_t offset, size_t size, bool to_gpu, Transfer &&transfer) {
        if (!valid_ || !range(offset, size)) return false;
        if (!size) return true;
        const size_t first = offset / PageSize, end = (offset + size - 1) / PageSize + 1;
        const Owner source = to_gpu ? Owner::Cpu : Owner::Gpu;
        bool copied = false;
        for (size_t p = first; p < end;) {
            if (owner_[p] != source) { ++p; continue; }
            const size_t begin = p;
            while (p < end && owner_[p] == source) ++p;
            // transfer is synchronous: true means the fence completed, and
            // CPU visibility (for downloads) was established. Failure poisons
            // the ledger rather than publishing uncompleted contents.
            if (!transfer(begin * PageSize, (p - begin) * PageSize)) { fail(); return false; }
            for (size_t q = begin; q < p; ++q) owner_[q] = Owner::Both;
            (to_gpu ? uploaded : downloaded) += (p - begin) * PageSize;
            copied = true;
        }
        if (to_gpu && !copied) ++clean_uses;
        return true;
    }
    template<class Download> bool prepare_cpu_write(size_t offset, size_t size, Download &&download) {
        if (!valid_ || !range(offset, size)) return false;
        if (!size) return true;
        const size_t end = offset + size;
        for (size_t p = offset / PageSize; p <= (end - 1) / PageSize; ++p) {
            // A full-page overwrite can discard its GPU version; a partial
            // overwrite must first preserve the bytes outside the CPU write.
            const bool full = offset <= p * PageSize && end >= (p + 1) * PageSize;
            if (!full && owner_[p] == Owner::Gpu
                && !sync(p * PageSize, PageSize, false, download)) return false;
        }
        return true;
    }
    bool commit_cpu_write(size_t offset, size_t size) {
        if (!valid_ || !range(offset, size)) return false;
        if (!size) return true;
        const size_t end = offset + size;
        // Validate every page before changing any ownership.
        for (size_t p = offset / PageSize; p <= (end - 1) / PageSize; ++p)
            if (owner_[p] == Owner::Gpu
                && !(offset <= p * PageSize && end >= (p + 1) * PageSize)) return false;
        for (size_t p = offset / PageSize; p <= (end - 1) / PageSize; ++p) owner_[p] = Owner::Cpu;
        ++cpu_events;
        return true;
    }
    template<class Upload, class GpuWrite> bool gpu_write(size_t offset, size_t size,
        Upload &&upload, GpuWrite &&write) {
        if (!valid_ || !range(offset, size)) return false;
        if (!size) return true;
        const size_t end = offset + size;
        // GPU partial writes need the same preservation rule as CPU writes.
        for (size_t p = offset / PageSize; p <= (end - 1) / PageSize; ++p) {
            const bool full = offset <= p * PageSize && end >= (p + 1) * PageSize;
            if (!full && owner_[p] == Owner::Cpu
                && !sync(p * PageSize, PageSize, true, upload)) return false;
        }
        if (!write(offset, size)) { fail(); return false; }
        for (size_t p = offset / PageSize; p <= (end - 1) / PageSize; ++p) owner_[p] = Owner::Gpu;
        ++gpu_events;
        return true;
    }
};
} // namespace switch_validation::mirror
