// Automatic private GPU mirror experiment. No Guest mapping or Hash decision changes.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <util/switch_mirror_probe_state.h>
#if defined(__SWITCH__) && defined(VITA3K_SWITCH_TRACELAB)
#include <util/switch_direct_heap.h>
#include <util/switch_heap_fault_probe.h>
#include <util/switch_memory_validation.h>
#include <util/mirror_probe_controls.h>
#include <util/mirror_shader_contract.h>
#include <switch_mirror_shader_spv.h>
#include <util/switch_protected_cache_probe.h>
#include <xxhash.h>
#include <switch/arm/counter.h>
#include <vulkan/vulkan.hpp>
#include <malloc.h>
#include <array>
#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>

namespace switch_validation::mirror_probe {
inline void check(VkResult result, const char *where) {
    if (result != VK_SUCCESS) throw std::runtime_error(std::string(where) + " VkResult=" + std::to_string(int(result)));
}
template<class Renderer> class Transport {
    Renderer &renderer;
    VkDevice device;
    struct Buffer { VkBuffer buffer{}; VkDeviceMemory memory{}; void *mapped=nullptr; } upload_buffer, gpu_buffer, read_buffer, shader_scratch;
    VkImage image{}; VkDeviceMemory image_memory{};
    VkShaderModule shader{}; VkPipelineLayout shader_layout{}; VkPipeline shader_pipeline{};
    VkCommandPool pool{}; VkCommandBuffer command{}; VkFence fence{};
    uint8_t *allocation=nullptr;
    bool armed=false, in_flight=false, closed=false;
    static constexpr size_t size=mirror_shader::payload, guard=mirror_shader::guard, physical=mirror_shader::physical;
    auto &vk() { return VULKAN_HPP_DEFAULT_DISPATCHER; }
    uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags required, bool noncoherent=false) {
        const auto &p=renderer.physical_device_memory;
        for(uint32_t i=0;i<p.memoryTypeCount;++i) {
            const auto flags=static_cast<VkMemoryPropertyFlags>(p.memoryTypes[i].propertyFlags);
            if((bits&(1u<<i)) && (flags&required)==required && (!noncoherent || !(flags&VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)))return i;
        }
        throw std::runtime_error("required private buffer memory type unavailable");
    }
    void buffer(Buffer &b,bool mapped) {
        VkBufferCreateInfo info{}; info.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;info.size=physical;
        info.usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT
            |VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT|VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        info.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
        check(vk().vkCreateBuffer(device,&info,nullptr,&b.buffer),"create private buffer");
        VkMemoryRequirements req{};vk().vkGetBufferMemoryRequirements(device,b.buffer,&req);
        VkMemoryAllocateFlagsInfo flags{};flags.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;flags.flags=VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
        VkMemoryAllocateInfo alloc{};alloc.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;alloc.pNext=&flags;alloc.allocationSize=req.size;
        // Exercise the cached, NON-coherent path explicitly. Never assume the
        // coherent/unmaintained workaround type makes device writes visible.
        alloc.memoryTypeIndex=memory_type(req.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_CACHED_BIT,true);
        check(vk().vkAllocateMemory(device,&alloc,nullptr,&b.memory),"allocate private buffer");
        check(vk().vkBindBufferMemory(device,b.buffer,b.memory,0),"bind private buffer");
        if(mapped)check(vk().vkMapMemory(device,b.memory,0,VK_WHOLE_SIZE,0,&b.mapped),"map private buffer");
    }
    void flush(Buffer &b,bool invalidate) {
        VkMappedMemoryRange range{};range.sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;range.memory=b.memory;range.offset=0;range.size=VK_WHOLE_SIZE;
        check(invalidate?vk().vkInvalidateMappedMemoryRanges(device,1,&range):vk().vkFlushMappedMemoryRanges(device,1,&range),
            invalidate?"invalidate readback":"flush upload");
    }
    void barrier(VkAccessFlags source,VkAccessFlags dest,VkPipelineStageFlags a,VkPipelineStageFlags b) {
        VkMemoryBarrier m{};m.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER;m.srcAccessMask=source;m.dstAccessMask=dest;
        vk().vkCmdPipelineBarrier(command,a,b,0,1,&m,0,nullptr,0,nullptr);
    }
    void begin() {
        if(in_flight)throw std::runtime_error("private command still in flight");
        check(vk().vkResetCommandPool(device,pool,0),"reset private pool");
        check(vk().vkResetFences(device,1,&fence),"reset private fence");
        VkCommandBufferBeginInfo info{};info.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;info.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check(vk().vkBeginCommandBuffer(command,&info),"begin private command");
        // Includes previous shader producers and consumers, not only transfers.
        const auto access=VK_ACCESS_TRANSFER_WRITE_BIT|VK_ACCESS_TRANSFER_READ_BIT
            |VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT;
        const auto stages=VK_PIPELINE_STAGE_TRANSFER_BIT|VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        barrier(access,access,stages,stages);
    }
    void submit() {
        check(vk().vkEndCommandBuffer(command),"end private command");
        VkSubmitInfo info{};info.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO;info.commandBufferCount=1;info.pCommandBuffers=&command;
        {
            const std::lock_guard<std::mutex> lock(renderer.general_queue_mutex);
            // Treat any uncertain submission as in-flight until a fence proves
            // completion. A timeout retains resources and blocks chainload.
            in_flight=true;
            check(vk().vkQueueSubmit(static_cast<VkQueue>(renderer.general_queue),1,&info,fence),"submit private command");
        }
        ++waits;
        const auto result=vk().vkWaitForFences(device,1,&fence,VK_TRUE,5'000'000'000ull);
        if(result==VK_ERROR_DEVICE_LOST)renderer.device_lost.store(true,std::memory_order_release);
        check(result,"wait private fence");in_flight=false;
    }
    void copy(VkBuffer from,VkBuffer to,size_t offset,size_t bytes) {
        VkBufferCopy region{};region.srcOffset=offset;region.dstOffset=offset;region.size=bytes;
        vk().vkCmdCopyBuffer(command,from,to,1,&region);
    }
    VkDeviceAddress address(const Buffer &b) {
        VkBufferDeviceAddressInfo info{};info.sType=VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;info.buffer=b.buffer;
        const auto result=vk().vkGetBufferDeviceAddress ? vk().vkGetBufferDeviceAddress(device,&info) : 0;
        if(!result)throw std::runtime_error("private shader buffer has no device address");
        return result;
    }
    void setup_shader() {
        if(renderer.physical_device_properties.apiVersion<VK_API_VERSION_1_1)
            throw std::runtime_error("private BDA shader requires Vulkan 1.1 / SPIR-V 1.3");
        if(renderer.general_family_index>=renderer.physical_device_queue_families.size()
            || !(static_cast<VkQueueFlags>(renderer.physical_device_queue_families[renderer.general_family_index].queueFlags)&VK_QUEUE_COMPUTE_BIT))
            throw std::runtime_error("selected queue does not support compute");
        VkShaderModuleCreateInfo sm{};sm.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        sm.codeSize=sizeof(mirror_shader::spirv);sm.pCode=mirror_shader::spirv;
        check(vk().vkCreateShaderModule(device,&sm,nullptr,&shader),"create BDA probe shader");
        VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(mirror_shader::Push)};
        VkPipelineLayoutCreateInfo pl{};pl.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pl.pushConstantRangeCount=1;pl.pPushConstantRanges=&range;
        check(vk().vkCreatePipelineLayout(device,&pl,nullptr,&shader_layout),"create BDA probe layout");
        VkComputePipelineCreateInfo pi{};pi.sType=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        pi.stage.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        pi.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;pi.stage.module=shader;pi.stage.pName="main";pi.layout=shader_layout;
        check(vk().vkCreateComputePipelines(device,VK_NULL_HANDLE,1,&pi,nullptr,&shader_pipeline),"create BDA probe pipeline");
        const auto detail=std::string("SPIR-V sha256=")+mirror_shader::spirv_sha256
            +"; private BDA compute fill/copy; compiled and spirv-val checked on host";
        record("gpu_mirror","shader_program","pass",detail.c_str());
    }
    void dispatch(Buffer &from,Buffer &to,size_t offset,size_t bytes,uint32_t value,bool copying) {
        if(!mirror_shader::valid_range(offset,bytes))throw std::runtime_error("invalid private shader range");
        const mirror_shader::Push push{address(from)+guard+offset,address(to)+guard+offset,
            static_cast<uint32_t>(bytes/4),value,copying?1u:0u,0};
        vk().vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,shader_pipeline);
        vk().vkCmdPushConstants(command,shader_layout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(push),&push);
        vk().vkCmdDispatch(command,mirror_shader::groups(push.count),1,1);
        ++shader_dispatches;
        if(copying)++shader_copies;else ++shader_fills;
    }
    bool check_gpu_guards() {
        size_t bad=0;
        ++guard_checks;
        if(!mirror_shader::guards_intact({static_cast<const uint8_t*>(read_buffer.mapped),physical},&bad))
            throw std::runtime_error("private GPU guard modified at byte "+std::to_string(bad));
        return true;
    }
    bool shader_chain(std::array<uint8_t,size> &result) {
        const auto before=waits;
        begin();
        vk().vkCmdFillBuffer(command,shader_scratch.buffer,0,physical,0xa5a5a5a5u);
        barrier(VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        for(const auto range:mirror_shader::copies) {
            barrier(VK_ACCESS_SHADER_WRITE_BIT|VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
            if(range.bytes)vk().vkCmdFillBuffer(command,shader_scratch.buffer,guard+range.offset,range.bytes,0x3c3c3c3cu);
            barrier(VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
            // Zero length uses a poison FILL, so removing the shader bound
            // check corrupts the guard rather than copying identical sentinels.
            dispatch(gpu_buffer,shader_scratch,range.offset,range.bytes,0xdeadbeefu,range.bytes!=0);
            barrier(VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        }
        // Poison the original AFTER all shader reads. A missing copy-back must
        // fail the independent oracle, not be hidden by the old correct buffer.
        barrier(VK_ACCESS_SHADER_READ_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
        vk().vkCmdFillBuffer(command,gpu_buffer.buffer,guard,size,0x7e7e7e7eu);
        barrier(VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        dispatch(shader_scratch,gpu_buffer,0,size,0,true);
        barrier(VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
        copy(shader_scratch.buffer,read_buffer.buffer,0,physical);
        barrier(VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_HOST_READ_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT);
        submit();flush(read_buffer,true);check_gpu_guards();
        std::memcpy(result.data(),static_cast<const uint8_t*>(read_buffer.mapped)+guard,size);
        if(waits!=before+1)throw std::runtime_error("shader chain inserted an intermediate CPU fence");
        ++shader_chains;
        return true;
    }
    bool disarm() {
        if(!armed)return heap_fault_safe_to_chainload();
        const auto r=heap_fault_disarm();armed=false;faults+=r.faults;
        return r.restored && !r.restore_failed_mask && heap_fault_safe_to_chainload();
    }
public:
    uint64_t uploaded=0,downloaded=0,faults=0,waits=0;
    uint64_t shader_dispatches=0,shader_fills=0,shader_copies=0,shader_chains=0,guard_checks=0;
    explicit Transport(Renderer &r):renderer(r),device(static_cast<VkDevice>(r.device)){}
    ~Transport(){close();}
    uint8_t *cpu(){return allocation?allocation+4096:nullptr;}
    void setup(){
        allocation=static_cast<uint8_t*>(memalign(4096,size+8192));
        if(!allocation)throw std::bad_alloc();
        std::memset(allocation,0xa5,size+8192);
        buffer(upload_buffer,true);buffer(gpu_buffer,false);buffer(read_buffer,true);buffer(shader_scratch,false);
        VkBufferDeviceAddressInfo ai{};ai.sType=VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;ai.buffer=gpu_buffer.buffer;
        if(!vk().vkGetBufferDeviceAddress || !vk().vkGetBufferDeviceAddress(device,&ai))throw std::runtime_error("private mirror has no device address");
        VkImageCreateInfo im{};im.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;im.imageType=VK_IMAGE_TYPE_2D;
        im.format=VK_FORMAT_R8G8B8A8_UNORM;im.extent={64,64,1};im.mipLevels=1;im.arrayLayers=1;im.samples=VK_SAMPLE_COUNT_1_BIT;
        im.tiling=VK_IMAGE_TILING_OPTIMAL;im.usage=VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        im.sharingMode=VK_SHARING_MODE_EXCLUSIVE;im.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
        check(vk().vkCreateImage(device,&im,nullptr,&image),"create private image");
        VkMemoryRequirements req{};vk().vkGetImageMemoryRequirements(device,image,&req);
        VkMemoryAllocateInfo ia{};ia.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;ia.allocationSize=req.size;
        ia.memoryTypeIndex=memory_type(req.memoryTypeBits,0);
        check(vk().vkAllocateMemory(device,&ia,nullptr,&image_memory),"allocate private image");
        check(vk().vkBindImageMemory(device,image,image_memory,0),"bind private image");
        VkCommandPoolCreateInfo pi{};pi.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;pi.queueFamilyIndex=renderer.general_family_index;
        check(vk().vkCreateCommandPool(device,&pi,nullptr,&pool),"create private pool");
        VkCommandBufferAllocateInfo ci{};ci.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;ci.commandPool=pool;
        ci.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;ci.commandBufferCount=1;
        check(vk().vkAllocateCommandBuffers(device,&ci,&command),"allocate private command");
        VkFenceCreateInfo fi{};fi.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        check(vk().vkCreateFence(device,&fi,nullptr,&fence),"create private fence");
        setup_shader();
        begin();vk().vkCmdFillBuffer(command,gpu_buffer.buffer,0,physical,0xa5a5a5a5u);submit();
    }
    bool begin_case(){armed=heap_fault_arm(cpu(),size);return armed;}
    bool end_case(){return disarm();}
    bool cpu_write(size_t o,size_t n,uint8_t value){
        if(!armed||!n||o>size||n>size-o)return false;
        std::memset(cpu()+o,value,n); // real raw host write, not a fake notification
        const auto evidence=heap_fault_peek();
        uint32_t expected=0;for(size_t p=o/4096;p<=(o+n-1)/4096;++p)expected|=1u<<p;
        return !evidence.last_rc&&(evidence.dirty_mask&expected)==expected;
    }
    bool upload(size_t o,size_t n){
        std::memcpy(static_cast<uint8_t*>(upload_buffer.mapped)+guard+o,cpu()+o,n);flush(upload_buffer,false);
        begin();barrier(VK_ACCESS_HOST_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT,VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
        copy(upload_buffer.buffer,gpu_buffer.buffer,guard+o,n);submit();uploaded+=n;return true;
    }
    bool read_gpu(uint8_t *out){
        begin();copy(gpu_buffer.buffer,read_buffer.buffer,0,physical);
        barrier(VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_HOST_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT);
        submit();flush(read_buffer,true);check_gpu_guards();std::memcpy(out,static_cast<const uint8_t*>(read_buffer.mapped)+guard,size);return true;
    }
    bool download(size_t o,size_t n){
        begin();copy(gpu_buffer.buffer,read_buffer.buffer,guard+o,n);
        barrier(VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_HOST_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT);
        submit();flush(read_buffer,true);
        // Explicit ownership acquisition, not an accidental fault-based upload.
        if(!disarm())return false;
        std::memcpy(cpu()+o,static_cast<uint8_t*>(read_buffer.mapped)+guard+o,n);downloaded+=n;
        return begin_case();
    }
    bool gpu_fill(size_t o,size_t n,uint32_t value){
        if(!mirror_shader::valid_range(o,n))return false;
        const auto before=heap_fault_peek().faults;
        begin();dispatch(gpu_buffer,gpu_buffer,o,n,value,false);submit();
        // A GPU write affects only the independent mirror. CPU fault evidence
        // stays unchanged; ownership must come from the explicit GPU event.
        return heap_fault_peek().faults==before;
    }
    bool image_roundtrip(uint8_t *out){
        std::array<uint8_t,size> shader_result{};
        if(!shader_chain(shader_result))return false;
        begin();copy(gpu_buffer.buffer,read_buffer.buffer,0,physical);
        barrier(VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkImageMemoryBarrier image_barrier{};image_barrier.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        image_barrier.srcAccessMask=0;image_barrier.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
        image_barrier.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED;image_barrier.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        image_barrier.srcQueueFamilyIndex=image_barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        image_barrier.image=image;image_barrier.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        vk().vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&image_barrier);
        VkBufferImageCopy region{};region.bufferOffset=guard;region.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};region.imageExtent={64,64,1};
        vk().vkCmdCopyBufferToImage(command,gpu_buffer.buffer,image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&region);
        image_barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;image_barrier.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
        image_barrier.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;image_barrier.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        vk().vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&image_barrier);
        vk().vkCmdCopyImageToBuffer(command,image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,read_buffer.buffer,1,&region);
        barrier(VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_HOST_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT);
        submit();flush(read_buffer,true);check_gpu_guards();
        std::memcpy(out,static_cast<const uint8_t*>(read_buffer.mapped)+guard,size);
        // Controls also compares this result against its independent CPU oracle.
        return std::memcmp(out,shader_result.data(),size)==0;
    }
    bool close() noexcept {
        if(closed)return heap_fault_safe_to_chainload();
        closed=true;
        const bool rw=disarm();
        bool guards=rw;
        if(allocation&&rw)for(size_t i=0;i<4096;++i)guards&=allocation[i]==0xa5&&cpu()[size+i]==0xa5;
        if(in_flight){heap_fault_retain_allocation();return false;} // Never free a submitted resource on timeout.
        if(fence)vk().vkDestroyFence(device,fence,nullptr);
        if(pool)vk().vkDestroyCommandPool(device,pool,nullptr);
        if(shader_pipeline)vk().vkDestroyPipeline(device,shader_pipeline,nullptr);
        if(shader_layout)vk().vkDestroyPipelineLayout(device,shader_layout,nullptr);
        if(shader)vk().vkDestroyShaderModule(device,shader,nullptr);
        if(image)vk().vkDestroyImage(device,image,nullptr);
        if(image_memory)vk().vkFreeMemory(device,image_memory,nullptr);
        for(auto *b:{&upload_buffer,&gpu_buffer,&read_buffer,&shader_scratch}){
            if(b->mapped)vk().vkUnmapMemory(device,b->memory);
            if(b->buffer)vk().vkDestroyBuffer(device,b->buffer,nullptr);
            if(b->memory)vk().vkFreeMemory(device,b->memory,nullptr);
        }
        if(allocation&&rw)std::free(allocation);
        else if(allocation)heap_fault_retain_allocation();
        allocation=nullptr;return guards&&heap_fault_safe_to_chainload();
    }
};
template<class Renderer> struct Runner {
    Renderer *owner;Transport<Renderer> backend;std::unique_ptr<mirror::Controls<Transport<Renderer>>> controls;
    unsigned step=0;uint64_t start=0;
    explicit Runner(Renderer &r):owner(&r),backend(r){}
};
template<class Renderer> inline std::unique_ptr<Runner<Renderer>> &runner(){static std::unique_ptr<Runner<Renderer>> p;return p;}
template<class Renderer> void finish(Renderer &r,bool success){
    auto &p=runner<Renderer>();if(p&&p->owner!=&r)return;
    const bool clean=!p||p->backend.close();
    if(p){state.uploaded.store(p->backend.uploaded);state.downloaded.store(p->backend.downloaded);
        state.faults.store(p->backend.faults);state.waits.store(p->backend.waits);
        state.shader_dispatches.store(p->backend.shader_dispatches);state.shader_fills.store(p->backend.shader_fills);
        state.shader_copies.store(p->backend.shader_copies);state.shader_chains.store(p->backend.shader_chains);
        state.guard_checks.store(p->backend.guard_checks);
        state.audits.store(p->controls?p->controls->audits:0);state.ticks.store(armGetSystemTick()-p->start);}
    // Freeze metadata before normal renderer teardown releases logical imports.
    deferred_import::registry.freeze_window();
    const bool marked=heap_fault_checkpoint("gpu_mirror_returned",false);
    success=success&&marked&&shader_verified(snapshot());
    state.cleanup.store(clean);state.passed.store(success&&clean);
    record("gpu_mirror","cleanup_complete",clean?"pass":"fail",clean?"fences completed; private GPU objects released; CPU RW/guards verified":"resources retained; do not chainload or repeat");
    if(success&&clean)state.completed.store(case_count);
    record("gpu_mirror","suite_complete",success&&clean?"pass":"fail","private compute-BDA/transfer/image byte oracle; no live Guest Hash skip, attachment/IPC or concurrent-writer proof");
    p.reset();state.finished.store(true,std::memory_order_release);
}
template<class Renderer> void tick(Renderer &r){
    if(!heap_fault_probe_enabled()||!direct_heap::state.active.load()||state.finished.load())return;
    // Show the automatic-test HUD for one frame before any blocking work.
    if(!state.requested.exchange(true))return;
    if(!direct_heap::state.cache_finished.load())run_protected_cache_probe(&XXH3_64bits);
    if(!direct_heap::state.cache_finished.load())return;
    if(!direct_heap::state.cache_passed.load()||!heap_fault_safe_to_chainload())return; // Existing prerequisite failure stops the capture.
    const std::lock_guard<std::mutex> lock(direct_heap::scratch_probe_mutex);
    auto &p=runner<Renderer>();state.started.store(true);
    try {
        if(!p){
            if(!heap_fault_checkpoint("gpu_mirror_setup",true))throw std::runtime_error("durable checkpoint unavailable");
            p=std::make_unique<Runner<Renderer>>(r);p->start=armGetSystemTick();p->backend.setup();
            p->controls=std::make_unique<mirror::Controls<Transport<Renderer>>>(p->backend);
        }
        if(p->owner!=&r)throw std::runtime_error("GPU mirror owner changed");
        if(p->step==case_count-1){finish(r,true);return;}
        if(!heap_fault_checkpoint(names[p->step],true))throw std::runtime_error("case checkpoint unavailable");
        const auto case_start=armGetSystemTick();
        const bool armed=p->backend.begin_case();
        const bool passed=armed&&p->controls->step(p->step);
        const bool restored=p->backend.end_case();
        char detail[400];
        std::snprintf(detail,sizeof(detail),"armed=%u restored=%u uploads=%llu downloads=%llu audits=%llu faults=%llu waits=%llu mismatch=%lld expected=%u actual=%u; cumulative counters; real compute-BDA/transfer/image, private only",
            unsigned(armed),unsigned(restored),(unsigned long long)p->backend.uploaded,
            (unsigned long long)p->backend.downloaded,(unsigned long long)p->controls->audits,
            (unsigned long long)p->backend.faults,(unsigned long long)p->backend.waits,
            p->controls->mismatch==SIZE_MAX?-1ll:static_cast<long long>(p->controls->mismatch),
            unsigned(p->controls->expected_byte),unsigned(p->controls->actual_byte));
        record("gpu_mirror",names[p->step],passed&&restored?"pass":"fail",detail,armGetSystemTick()-case_start,1);
        state.completed.store(++p->step);state.uploaded.store(p->backend.uploaded);state.downloaded.store(p->backend.downloaded);
        state.audits.store(p->controls->audits);state.faults.store(p->backend.faults);state.waits.store(p->backend.waits);
        state.shader_dispatches.store(p->backend.shader_dispatches);state.shader_fills.store(p->backend.shader_fills);
        state.shader_copies.store(p->backend.shader_copies);state.shader_chains.store(p->backend.shader_chains);
        state.guard_checks.store(p->backend.guard_checks);
        if(!passed||!restored){finish(r,false);return;}
        if(!heap_fault_checkpoint("gpu_mirror_step_returned",false))throw std::runtime_error("case completion checkpoint failed");
    }catch(const std::exception &e){record("gpu_mirror","runtime_error","fail",e.what());finish(r,false);}
}
template<class Renderer> void shutdown(Renderer &r){
    const std::lock_guard<std::mutex> lock(direct_heap::scratch_probe_mutex);
    if(runner<Renderer>()&&runner<Renderer>()->owner==&r)finish(r,false);
}
} // namespace switch_validation::mirror_probe
#else
namespace switch_validation::mirror_probe {
template<class Renderer> inline void tick(Renderer &){}
template<class Renderer> inline void shutdown(Renderer &){}
}
#endif