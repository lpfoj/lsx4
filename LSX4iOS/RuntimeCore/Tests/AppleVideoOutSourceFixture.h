// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "core/libraries/videoout/buffer.h"

// Exercise real image-cache selection: a same-sized SNORM intermediate has
// storage-compatible bits but is not the registered UNORM/SRGB scanout.
static void TestAppleVideoOutSource(Core::MemoryManager* memory) {
    constexpr u32 bytes=64u<<10;
    const auto physical=memory->Allocate(0,memory->GetTotalDirectSize(),bytes,16u<<10,3);
    Require(physical!=static_cast<PAddr>(-1),"VideoOut fixture allocation failed");
    struct Cleanup {
        Core::MemoryManager* memory;PAddr physical;VAddr address{};
        ~Cleanup() {if(address)memory->UnmapMemory(address,bytes);memory->Free(physical,bytes,true);}
    } cleanup{memory,physical};
    void* mapping{};
    Require(memory->MapMemory(&mapping,memory->GetAddressSpace().UserVirtualBase()+(132ULL<<20),
        bytes,Core::MemoryProt::CpuReadWrite|Core::MemoryProt::GpuReadWrite,
        Core::MemoryMapFlags{},Core::VMAType::Direct,"videoout-format-fixture",false,
        physical,16u<<10)==0,"VideoOut fixture mapping failed");
    cleanup.address=reinterpret_cast<VAddr>(mapping);std::memset(mapping,0,bytes);
    Vulkan::Instance instance(false,false,true);
    Vulkan::Scheduler scheduler(instance);
    auto liverpool=std::make_unique<AmdGpu::Liverpool>();
    auto rasterizer=std::make_unique<Vulkan::Rasterizer>(instance,scheduler,liverpool.get());
    bool rejectedSigned=false,acceptedUnsigned=false;
    liverpool->SendCommand<true>([&] {
        auto& cache=rasterizer->GetTextureCache();
        Libraries::VideoOut::BufferAttributeGroup group{};
        group.attrib.pixel_format=Libraries::VideoOut::PixelFormat::A8B8G8R8Srgb;
        group.attrib.tiling_mode=Libraries::VideoOut::TilingMode::Linear;
        group.attrib.width=group.attrib.pitch_in_pixel=128;group.attrib.height=15;
        VideoCore::TextureCache::ImageDesc scanout(group,cleanup.address);
        auto preferred=cache.FindImage(scanout,true);
        auto candidate=scanout;
        candidate.info.guest_address=cleanup.address+(16u<<10);
        candidate.info.size.height=16;candidate.info.pixel_format=vk::Format::eR8G8B8A8Snorm;
        candidate.info.UpdateSize();candidate.type=VideoCore::TextureCache::BindingType::RenderTarget;
        auto signedId=cache.FindImage(candidate,true);
        auto& signedImage=cache.GetImage(signedId);
        signedImage.usage.render_target=1;signedImage.flags|=VideoCore::ImageFlagBits::GpuModified;
        cache.ExecutorTrackLiveDrawnColorTarget(signedId,signedImage.image_uid);
        auto selected=cache.ExecutorTakeLiveFullResolutionTarget(128,15,preferred);
        rejectedSigned=selected!=signedId;
        candidate.info.guest_address=cleanup.address+(32u<<10);
        candidate.info.pixel_format=vk::Format::eR8G8B8A8Unorm;
        auto unsignedId=cache.FindImage(candidate,true);
        auto& unsignedImage=cache.GetImage(unsignedId);
        unsignedImage.usage.render_target=1;unsignedImage.flags|=VideoCore::ImageFlagBits::GpuModified;
        cache.ExecutorTrackLiveDrawnColorTarget(unsignedId,unsignedImage.image_uid);
        selected=cache.ExecutorTakeLiveFullResolutionTarget(128,15,preferred);
        acceptedUnsigned=selected==unsignedId;
        scheduler.Finish();
    });
    std::fprintf(stderr,"[APPLE_VIDEOOUT_SOURCE_TEST] rejectedSigned=%d acceptedUnsigned=%d\n",
                 rejectedSigned,acceptedUnsigned);
    Require(rejectedSigned,"VideoOut selected unrelated SNORM intermediate");
    Require(acceptedUnsigned,"VideoOut lost UNORM/SRGB tile-height route");
}
