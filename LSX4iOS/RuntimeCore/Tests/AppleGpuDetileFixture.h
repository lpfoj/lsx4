// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "video_core/texture_cache/tile_manager.h"
#include "video_core/texture_cache/image_info.h"
#include <vector>

// Exercise the production compute pipeline and Metal buffer bindings against
// a fixed 8x8 thin-tile address vector, not another copy of the GLSL compiler.
// Two mip levels, two slices, and nonzero input offsets catch layout/stride
// errors which compiling SPIR-V alone cannot detect.
static void TestAppleGpuDetile(const Vulkan::Instance& instance,
                               Vulkan::Scheduler& scheduler) {
    constexpr std::array<u8,64> tile{
        0,1,4,5,16,17,20,21, 2,3,6,7,18,19,22,23,
        8,9,12,13,24,25,28,29, 10,11,14,15,26,27,30,31,
        32,33,36,37,48,49,52,53, 34,35,38,39,50,51,54,55,
        40,41,44,45,56,57,60,61, 42,43,46,47,58,59,62,63};
    VideoCore::StreamBuffer stream(instance,scheduler,VideoCore::MemoryUsage::Upload,1u<<20);
    VideoCore::TileManager tiler(instance,scheduler,stream);
    for (u32 bits : {8u,16u,32u,64u,128u}) {
        const u32 pixel_bytes=bits/8, bytes=640*pixel_bytes;
        std::vector<u8> tiled(bytes),expected(bytes);
        VideoCore::ImageInfo info{};
        info.props.is_tiled=1;
        info.num_bits=bits;info.num_samples=1;info.pitch=16;
        info.size={16,16,1};info.resources.levels=2;info.resources.layers=2;
        info.tile_mode=AmdGpu::TileMode::Thin1DThin;
        info.array_mode=AmdGpu::ArrayMode::Array1DTiledThin1;
        info.guest_size=bytes;
        u32 base=0;
        for (u32 mip=0;mip<2;++mip) {
            const u32 side=16>>mip, mip_bytes=side*side*2*pixel_bytes;
            info.mips_layout[mip]={mip_bytes,side,side,base};
            for (u32 slice=0;slice<2;++slice)
            for (u32 y=0;y<side;++y)
            for (u32 x=0;x<side;++x)
            for (u32 lane=0;lane<pixel_bytes;++lane) {
                const u32 linear=base+((slice*side+y)*side+x)*pixel_bytes+lane;
                const u32 block=(y/8)*(side/8)+x/8;
                const u32 swizzled=base+(slice*side*side+block*64+tile[(y%8)*8+x%8])*pixel_bytes+lane;
                const auto value=static_cast<u8>((linear*37+(linear>>8)*11+0x53)&255);
                expected[linear]=value;tiled[swizzled]=value;
            }
            base+=mip_bytes;
        }
        // Leave a prefix so this also verifies the descriptor's input offset.
        std::array<u8,256> prefix{};stream.Copy(prefix.data(),prefix.size(),256);
        const auto offset=stream.Copy(tiled.data(),tiled.size(),instance.StorageMinAlignment());
        const auto result=tiler.DetileImage(stream.Handle(),static_cast<u32>(offset),info);
        VideoCore::Buffer download(instance,scheduler,VideoCore::MemoryUsage::Download,
                                   0,VideoCore::AllFlags,bytes);
        auto command=scheduler.CommandBuffer();
        command.copyBuffer(result.buffer,download.Handle(),vk::BufferCopy{result.offset,0,bytes});
        const auto barrier=download.GetBarrier(vk::AccessFlagBits2::eHostRead,
                                               vk::PipelineStageFlagBits2::eHost);
        Require(barrier.has_value(),"Detile fixture has no readback barrier");
        command.pipelineBarrier2(vk::DependencyInfo{
            .bufferMemoryBarrierCount=1,.pBufferMemoryBarriers=&*barrier});
        tiler.RetireDetileResult(result);
        scheduler.Finish();download.InvalidateMappedRange(0,bytes);
        Require(download.mapped_data.size()>=bytes,"Detile readback is unmapped");
        for(u32 index=0;index<bytes;++index) {
            if(download.mapped_data[index]!=expected[index]) {
                std::fprintf(stderr,"[APPLE_GPU_DETILE_MISMATCH] bits=%u byte=%u actual=%u expected=%u\n",
                    bits,index,download.mapped_data[index],expected[index]);
                throw std::runtime_error("Production GPU detiler changed texture bytes");
            }
        }
        std::fprintf(stderr,"[APPLE_GPU_DETILE_TEST] bits=%u bytes=%u mips=2 slices=2 inputOffset=%llu exact=1\n",
            bits,bytes,offset);
    }
}
