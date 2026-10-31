// Automatic private-input controls using the renderer's actual digest callback.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <util/resource_hash_reuse.h>
#include <array>
namespace switch_validation::reuse {
inline bool dynamic_controls(uint64_t (*hash)(const void *,size_t)) {
    if (!hash) return false;
    std::array<uint8_t,4096> pixels{}; std::array<uint8_t,64> palette{};
    HashCache<8> cache;
    const Input input{reinterpret_cast<uintptr_t>(pixels.data()),reinterpret_cast<uintptr_t>(palette.data()),pixels.size(),palette.size(),1};
    Evidence proof;proof.valid=proof.publication_drained=proof.gpu_host_visible=true;
    proof.allocation=proof.lifetime=proof.mapping=proof.publication=1;proof.coverage=CompleteCoverage;
    const auto snap=[&]() noexcept {return proof;};
    unsigned reads=0;const auto digest=[&](){++reads;return hash(pixels.data(),pixels.size())^hash(palette.data(),palette.size());};
    cache.begin();cache.phase(1,Mode::Audit);
    const auto first=cache.evaluate(input,snap,digest);
    if(cache.evaluate(input,snap,digest)!=first || cache.report().total.audits!=1)return false;
    cache.phase(2,Mode::Performance);const auto before=reads;
    for(unsigned i=0;i<512;++i)if(cache.evaluate(input,snap,digest)!=first)return false;
    if(reads!=before)return false;
    // A full calculation follows each independent pixel/palette/version event.
    for(unsigned i=0;i<8;++i){
        if(i==0)pixels.front()^=9;
        if(i==1)pixels[pixels.size()/2]^=7;
        if(i==2)pixels.back()^=3;
        if(i==3)palette.back()^=5;
        if(i==4)++proof.host; // same-value output
        if(i==5)++proof.mapping;
        if(i==6)++proof.lifetime;
        ++proof.cpu;const auto n=reads;
        const auto expected=hash(pixels.data(),pixels.size())^hash(palette.data(),palette.size());
        if(cache.evaluate(input,snap,digest)!=expected || reads!=n+1)return false;
    }
    proof.host_busy=true;const auto n=reads;cache.evaluate(input,snap,digest);
    if(reads!=n+1)return false;
    proof.host_busy=false;cache.phase(4,Mode::Audit);
    cache.evaluate(input,snap,digest);cache.evaluate(input,snap,digest);
    pixels[31]^=1; // deliberately absent writer evidence: must be detected
    cache.evaluate(input,snap,digest);
    if(!cache.failed() || cache.report().total.mismatches!=1)return false;
    const auto last=reads;cache.phase(3,Mode::Performance);cache.evaluate(input,snap,digest);
    return reads==last+1; // disabled means full Hash, never stale reuse
}
} // namespace switch_validation::reuse
