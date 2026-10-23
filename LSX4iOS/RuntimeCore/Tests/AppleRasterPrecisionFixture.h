// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
// Synthetic native-Metal pixel-center fixture; no game code or game data.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <cstdio>
#include <cstring>
#include <vector>

extern "C" int LSX4RunAppleRasterPrecisionFixture() {
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        NSError* error = nil;
        NSString* source = @"#include <metal_stdlib>\nusing namespace metal;\n"
            "struct V {float2 p;float2 uv;}; struct O {float4 p [[position]];float2 uv;};\n"
            "vertex O vs(uint i [[vertex_id]],const device V* v [[buffer(0)]],"
            "constant float4& c [[buffer(1)]],constant float& bias [[buffer(2)]]) {\n"
            "float x=fma(1.f/c.z,fma(-c.z,1.f/c.x,2.f*v[i].p.x),-1.f);\n"
            "float y=fma(-2.f,v[i].p.y*(1.f/c.w),1.f+1.f/c.y);\n"
            "return {float4(x+2.f*bias/c.z,y-2.f*bias/c.w,0,1),v[i].uv};}\n"
            "fragment float4 fs(O v [[stage_in]],texture2d<float> t [[texture(0)]]) {"
            "constexpr sampler s(coord::normalized,filter::nearest,address::repeat);"
            "return t.sample(s,v.uv);}";
        MTLCompileOptions* options = [MTLCompileOptions new];
        options.fastMathEnabled = NO;
        id<MTLLibrary> library = [device newLibraryWithSource:source options:options error:&error];
        if (!library) {
            fprintf(stderr, "[APPLE_RASTER_PRECISION] compileError=%s\n", error.description.UTF8String);
            return 1;
        }
        auto descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
            width:432 height:240 mipmapped:NO];
        descriptor.usage = MTLTextureUsageRenderTarget;
        descriptor.storageMode = MTLStorageModeShared;
        auto color = [device newTextureWithDescriptor:descriptor];
        descriptor.width = 1024;
        descriptor.height = 376;
        descriptor.usage = MTLTextureUsageShaderRead;
        auto texture = [device newTextureWithDescriptor:descriptor];
        if (!color || !texture) return 2;
        const char* glyph[] = {"#######", "#......", "#......", "#######",
                               "......#", "......#", "......#", "#######"};
        std::vector<unsigned char> atlas(1024 * 376 * 4);
        for (int y = 0; y < 8; ++y) {
            for (int x = 0; x < 7; ++x) {
                if (glyph[y][x] == '#') {
                    memset(atlas.data() + ((y + 356) * 1024 + x + 624) * 4, 255, 4);
                }
            }
        }
        [texture replaceRegion:MTLRegionMake2D(0, 0, 1024, 376) mipmapLevel:0
            withBytes:atlas.data() bytesPerRow:4096];
        auto pipelineDescriptor = [MTLRenderPipelineDescriptor new];
        pipelineDescriptor.vertexFunction = [library newFunctionWithName:@"vs"];
        pipelineDescriptor.fragmentFunction = [library newFunctionWithName:@"fs"];
        pipelineDescriptor.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
        auto pipeline = [device newRenderPipelineStateWithDescriptor:pipelineDescriptor error:&error];
        if (!pipeline) return 3;
        auto queue = [device newCommandQueue];
        const float constants[] = {432, 240, 432, 240};
        const float vertices[] = {
            182.5f, 172, 624.f / 1024, 356.f / 376,
            189.5f, 172, 631.f / 1024, 356.f / 376,
            182.5f, 180, 624.f / 1024, 364.f / 376,
            189.5f, 180, 631.f / 1024, 364.f / 376,
        };
        // The shader correction is an independent reference. The viewport route
        // is the implementation used by the Vulkan bridge. Zero is a diagnostic
        // control; positive0.5 is the AMD PIX_CENTER=0 contract.
        for (bool viewportRoute : {false, true}) {
            for (float offset : {0.f, 0.5f}) {
                auto command = [queue commandBuffer];
                auto pass = [MTLRenderPassDescriptor renderPassDescriptor];
                pass.colorAttachments[0].texture = color;
                pass.colorAttachments[0].loadAction = MTLLoadActionClear;
                pass.colorAttachments[0].storeAction = MTLStoreActionStore;
                auto encoder = [command renderCommandEncoderWithDescriptor:pass];
                [encoder setRenderPipelineState:pipeline];
                const float shaderOffset = viewportRoute ? 0.f : offset;
                const double viewportOffset = viewportRoute ? offset : 0.;
                [encoder setViewport:MTLViewport{viewportOffset, viewportOffset, 432, 240, 0, 1}];
                [encoder setVertexBytes:vertices length:sizeof(vertices) atIndex:0];
                [encoder setVertexBytes:constants length:sizeof(constants) atIndex:1];
                [encoder setVertexBytes:&shaderOffset length:sizeof(shaderOffset) atIndex:2];
                [encoder setFragmentTexture:texture atIndex:0];
                [encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
                [encoder endEncoding];
                [command commit];
                [command waitUntilCompleted];
                if (command.status != MTLCommandBufferStatusCompleted) return 4;
                std::vector<unsigned char> pixels(432 * 240 * 4);
                [color getBytes:pixels.data() bytesPerRow:432 * 4
                    fromRegion:MTLRegionMake2D(0, 0, 432, 240) mipmapLevel:0];
                unsigned mismatches = 0;
                // Include a transparent border to catch a shift into another row
                // or column, not only the pixels inside the glyph rectangle.
                for (int y = -1; y <= 8; ++y) {
                    for (int x = -1; x <= 7; ++x) {
                        const bool expected = x >= 0 && x < 7 && y >= 0 && y < 8 && glyph[y][x] == '#';
                        const bool actual = pixels[((y + 172) * 432 + x + 182) * 4] > 128;
                        mismatches += actual != expected;
                    }
                }
                fprintf(stderr, "[APPLE_RASTER_PRECISION] route=%s bias=%.9g glyph=7x8 mismatchedPixels=%u\n",
                        viewportRoute ? "viewport" : "shader", offset, mismatches);
                if (offset > 0 && mismatches != 0) return 5;
            }
        }
        return 0;
    }
}
