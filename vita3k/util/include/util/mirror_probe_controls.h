// Same ownership control driver for the real Vulkan device and host adapter.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <util/mirror_page_state.h>
#include <array>
#include <cstring>
namespace switch_validation::mirror {
template<class Backend> class Controls {
    Backend &b;
    PageState<4> pages;
    std::array<uint8_t,16384> expected{}, observed{};
    bool upload(size_t o,size_t n){return b.upload(o,n);}
    bool download(size_t o,size_t n){return b.download(o,n);}
    bool acquire_gpu(){return pages.sync(0,expected.size(),true,[&](size_t o,size_t n){return upload(o,n);});}
    bool cpu_write(size_t o,size_t n,uint8_t value){
        if(!pages.prepare_cpu_write(o,n,[&](size_t a,size_t z){return download(a,z);}))return false;
        if(!b.cpu_write(o,n,value))return false;
        std::memset(expected.data()+o,value,n);
        return pages.commit_cpu_write(o,n);
    }
    bool gpu_write(size_t o,size_t n,uint8_t value){
        const bool ok=pages.gpu_write(o,n,[&](size_t a,size_t z){return upload(a,z);},
            [&](size_t a,size_t z){return b.gpu_fill(a,z,uint32_t(value)*0x01010101u);});
        if(ok)std::memset(expected.data()+o,value,n);
        return ok;
    }
    bool equal(const uint8_t *actual){
        ++audits;
        for(size_t i=0;i<expected.size();++i)if(actual[i]!=expected[i]){
            mismatch=i;expected_byte=expected[i];actual_byte=actual[i];return false;
        }
        return true;
    }
    bool audit_gpu(){
        if(!acquire_gpu()||!b.read_gpu(observed.data()))return false;
        return equal(observed.data());
    }
public:
    uint64_t audits=0;
    size_t mismatch=SIZE_MAX;uint8_t expected_byte=0,actual_byte=0;
    explicit Controls(Backend &backend):b(backend){
        for(size_t i=0;i<expected.size();++i)expected[i]=uint8_t(i*17u+3u);
        std::memcpy(b.cpu(),expected.data(),expected.size());
    }
    uint64_t uploaded()const{return pages.uploaded;}
    uint64_t downloaded()const{return pages.downloaded;}
    bool step(unsigned index){
        switch(index){
        case 0:return audit_gpu();
        case 1:{const auto up=pages.uploaded,down=pages.downloaded;
            for(unsigned i=0;i<512;++i)if(!acquire_gpu())return false;
            return up==pages.uploaded&&down==pages.downloaded&&audit_gpu();}
        case 2:return cpu_write(0,4,0x45)&&audit_gpu();
        case 3:return cpu_write(3*4096+4,4,0x67)&&audit_gpu();
        case 4:return gpu_write(4096,4096,0x89)&&audit_gpu();
        case 5:{const auto down=pages.downloaded;
            return cpu_write(4096+128,4,0xab)&&pages.downloaded==down+4096&&audit_gpu();}
        case 6:{ // Same CPU bytes as the old shadow must overwrite a NEW GPU version.
            if(!cpu_write(8192,4096,0)||!audit_gpu()||!gpu_write(8192,4096,0xcc))return false;
            const auto down=pages.downloaded,up=pages.uploaded;
            return cpu_write(8192,4096,0)&&audit_gpu()&&pages.uploaded==up+4096&&pages.downloaded==down;}
        case 7:return cpu_write(8192-8,16,0xde)&&audit_gpu();
        case 8:{ // Preserve a CPU edit outside a later partial GPU write.
            const auto up=pages.uploaded;
            return cpu_write(256,4,0x12)&&gpu_write(512,16,0x34)&&audit_gpu()&&pages.uploaded==up+4096;}
        case 9:if(!gpu_write(4096,4096,0x56)||!pages.sync(0,expected.size(),false,
                [&](size_t o,size_t n){return download(o,n);}))return false;
            return equal(b.cpu());
        case 10:if(!acquire_gpu()||!b.image_roundtrip(observed.data()))return false;
            return equal(observed.data());
        case 11:{ // Explicit new logical lifetime at the same private CPU address.
            if(!b.cpu_write(0,expected.size(),0x78))return false;
            expected.fill(0x78);pages=PageState<4>{};return audit_gpu();}
        case 12:if(!audit_gpu()||!pages.sync(0,expected.size(),false,
                [&](size_t o,size_t n){return download(o,n);}))return false;
            return equal(b.cpu());
        default:return false;
        }
    }
};
} // namespace switch_validation::mirror
