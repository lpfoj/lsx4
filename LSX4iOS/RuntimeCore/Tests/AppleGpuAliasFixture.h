// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "video_core/amdgpu/liverpool.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"

// Two guest VAs share one physical page. A GPU write through the first VA must
// be published before the second VA is uploaded, including when the consumer
// already pins the memory topology. Exercise the real cache and GPU copy path.
static void TestAppleGpuAliasReadback(Core::MemoryManager* memory) {
    constexpr u32 bytes=16u<<10;
    const auto physical=memory->Allocate(0,memory->GetTotalDirectSize(),bytes,bytes,3);
    Require(physical!=static_cast<PAddr>(-1),"GPU alias allocation failed");
    struct Cleanup {
        Core::MemoryManager* memory;PAddr physical;VAddr first{},second{};
        ~Cleanup() {
            if(second) memory->UnmapMemory(second,bytes);
            if(first) memory->UnmapMemory(first,bytes);
            memory->Free(physical,bytes,true);
        }
    } cleanup{memory,physical};
    const auto protection=Core::MemoryProt::CpuReadWrite|Core::MemoryProt::GpuReadWrite;
    void* first{};void* second{};
    Require(memory->MapMemory(&first,memory->GetAddressSpace().UserVirtualBase()+(128ULL<<20),
        bytes,protection,Core::MemoryMapFlags{},Core::VMAType::Direct,
        "gpu-alias-source",false,physical,bytes)==0,"GPU source mapping failed");
    cleanup.first=reinterpret_cast<VAddr>(first);
    Require(memory->MapMemory(&second,cleanup.first+(2ULL<<20),bytes,protection,
        Core::MemoryMapFlags{},Core::VMAType::Direct,"gpu-alias-consumer",false,
        physical,bytes)==0,"GPU consumer mapping failed");
    cleanup.second=reinterpret_cast<VAddr>(second);
    Require(cleanup.second>=cleanup.first+(2ULL<<20),"GPU alias separation was not honored");
    std::memset(first,0x5a,bytes);
    Vulkan::Instance instance(false,false,true);
    Vulkan::Scheduler scheduler(instance);
    auto liverpool=std::make_unique<AmdGpu::Liverpool>();
    auto rasterizer=std::make_unique<Vulkan::Rasterizer>(instance,scheduler,liverpool.get());
    for(bool borrowedGuard : {false,true}) {
    const u32 pattern=borrowedGuard?0x71625384:0x28374615;
    bool sourceValid=false,consumerValid=false,backingMatches=false,gpuMatches=false;
    liverpool->SendCommand<true>([&] {
        auto& cache=rasterizer->GetBufferCache();
        const auto [source,sourceOffset]=cache.ObtainBuffer(cleanup.first,bytes,true);
        sourceValid=source && source->CpuAddr()!=0;
        if(!sourceValid) return;
        source->Fill(sourceOffset,bytes,pattern);
        std::fprintf(stderr,"[APPLE_GPU_ALIAS_TEST] phase=before-guarded-consumer borrowed=%d\n",borrowedGuard);
        {
            std::optional<Core::MemoryManager::SnapshotReadGuard> guard;
            if(borrowedGuard) guard.emplace(memory->AcquireSnapshotReadGuard());
            const auto [consumer,consumerOffset]=cache.ObtainBuffer(cleanup.second,bytes,
                false,false,{},false,false,false,guard?&*guard:nullptr);
            consumerValid=consumer && consumer->CpuAddr()!=0;
            if(!consumerValid) return;
            VideoCore::Buffer download(instance,scheduler,VideoCore::MemoryUsage::Download,
                0,VideoCore::AllFlags,bytes);
            auto command=scheduler.CommandBuffer();
            command.copyBuffer(consumer->Handle(),download.Handle(),
                               vk::BufferCopy{consumerOffset,0,bytes});
            const auto barrier=download.GetBarrier(vk::AccessFlagBits2::eHostRead,
                                                   vk::PipelineStageFlagBits2::eHost);
            if(!barrier) return;
            command.pipelineBarrier2(vk::DependencyInfo{
                .bufferMemoryBarrierCount=1,.pBufferMemoryBarriers=&*barrier});
            scheduler.Finish();download.InvalidateMappedRange(0,bytes);
            std::array<u32,bytes/4> copied{};
            backingMatches=guard
                ? memory->TryCopyBackingMemory(*guard,cleanup.second,copied.data(),bytes)
                : memory->TryCopyBackingMemory(cleanup.second,copied.data(),bytes);
            gpuMatches=download.mapped_data.size()>=bytes;
            for(u32 i=0;i<bytes/4;++i) {
                backingMatches &= copied[i]==pattern;
                u32 value{};if(gpuMatches) std::memcpy(&value,download.mapped_data.data()+4*i,4);
                gpuMatches &= value==pattern;
            }
        }
        std::fprintf(stderr,"[APPLE_GPU_ALIAS_TEST] phase=after-guarded-consumer borrowed=%d backing=%d gpu=%d\n",
                     borrowedGuard,backingMatches,gpuMatches);
    });
    Require(sourceValid && consumerValid && backingMatches && gpuMatches,
            "GPU alias publication changed backing or consumer bytes");
    }
}
