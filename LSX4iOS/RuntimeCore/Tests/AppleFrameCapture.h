// SPDX-License-Identifier: GPL-2.0-or-later
// Apple fixture only. Captures completed drawable pixels before presentation.
// MoltenVK is statically linked in this diagnostic, so its native texture
// accessor is available without a Vulkan loader or an optional extension route.
#import <Metal/Metal.h>
#import <Foundation/Foundation.h>
#import <CoreGraphics/CoreGraphics.h>
#import <ImageIO/ImageIO.h>
#import <objc/runtime.h>
#include <cstdio>
#include <cstdlib>
#include <limits.h>
extern "C" void vkGetMTLTextureMVK(void* image, id<MTLTexture>* texture);
static void CaptureAppleFrame(void* image) noexcept {
    @autoreleasepool {
        id<MTLBuffer> pixels=nil;
        id<MTLCommandQueue> queue=nil;
        CGDataProviderRef provider=nullptr;
        CGColorSpaceRef space=nullptr;
        CGImageRef bitmap=nullptr;
        CGImageDestinationRef destination=nullptr;
        char path[PATH_MAX]={};
        char temporary[PATH_MAX]={};
        bool saved=false;
        @try {
        id<MTLTexture> texture=nil;
        vkGetMTLTextureMVK(image,&texture);
        if (!texture || texture.framebufferOnly ||
            (texture.pixelFormat!=MTLPixelFormatBGRA8Unorm &&
             texture.pixelFormat!=MTLPixelFormatBGRA8Unorm_sRGB)) {
            std::fprintf(stderr,"[APPLE_CAPTURE_FAILED] unsupported_texture\n");return;
        }
        const auto width=texture.width, height=texture.height;
        if (!width || !height || width>(NSUIntegerMax-255)/4) {
            std::fprintf(stderr,"[APPLE_CAPTURE_FAILED] invalid_extent\n");return;
        }
        const auto stride=(width*4+255)&~NSUInteger(255);
        if (height>NSUIntegerMax/stride) {
            std::fprintf(stderr,"[APPLE_CAPTURE_FAILED] extent_overflow\n");return;
        }
        pixels=[texture.device newBufferWithLength:stride*height
                                                        options:MTLResourceStorageModeShared];
        queue=[texture.device newCommandQueue];
        id<MTLCommandBuffer> commands=[queue commandBuffer];
        id<MTLBlitCommandEncoder> blit=[commands blitCommandEncoder];
        if(!pixels || !queue || !commands || !blit) {
            std::fprintf(stderr,"[APPLE_CAPTURE_FAILED] Metal capture allocation unavailable bytes=%lu buffer=%d queue=%d commands=%d encoder=%d\n",
                stride*height,pixels!=nil,queue!=nil,commands!=nil,blit!=nil);
            if(blit) [blit endEncoding];
            return;
        }
        [blit copyFromTexture:texture sourceSlice:0 sourceLevel:0
                sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(width,height,1)
                    toBuffer:pixels destinationOffset:0 destinationBytesPerRow:stride
      destinationBytesPerImage:stride*height];
        [blit endEncoding];[commands commit];[commands waitUntilCompleted];
        if(commands.status!=MTLCommandBufferStatusCompleted) {
            std::fprintf(stderr,"[APPLE_CAPTURE_FAILED] %s\n",commands.error.description.UTF8String);
            return;
        }
        // Borrow the completed readback buffer until every ImageIO/CG object is
        // released below. A full NSData copy can itself exhaust the host heap.
        if (!pixels.contents) {
            std::fprintf(stderr,"[APPLE_CAPTURE_FAILED] missing_buffer_contents\n");return;
        }
        provider=CGDataProviderCreateWithData(nullptr,pixels.contents,stride*height,nullptr);
        space=CGColorSpaceCreateDeviceRGB();
        if (!provider || !space) {
            std::fprintf(stderr,"[APPLE_CAPTURE_FAILED] image_provider_unavailable\n");return;
        }
        bitmap=CGImageCreate(width,height,8,32,stride,space,
            kCGBitmapByteOrder32Little|kCGImageAlphaPremultipliedFirst,provider,nullptr,false,
            kCGRenderingIntentDefault);
        if (!bitmap) {
            std::fprintf(stderr,"[APPLE_CAPTURE_FAILED] bitmap_unavailable\n");return;
        }
        static unsigned sequence=0;
        const char* directory=std::getenv("LSX4_PROBE_CAPTURE_DIR");
        if (!directory || !*directory) {
            std::fprintf(stderr,"[APPLE_CAPTURE_FAILED] missing_directory\n");return;
        }
        const int pathLength=std::snprintf(path,sizeof(path),"%s/frame-%03u.png",directory,++sequence);
        const int temporaryLength=std::snprintf(temporary,sizeof(temporary),"%s.tmp",path);
        if (pathLength<0 || pathLength>=int(sizeof(path)) ||
            temporaryLength<0 || temporaryLength>=int(sizeof(temporary))) {
            temporary[0]=0;
            std::fprintf(stderr,"[APPLE_CAPTURE_FAILED] path_too_long\n");return;
        }
        // ImageIO writes to a temporary file instead of retaining a second
        // complete PNG in NSMutableData. Only publish a finalized image.
        NSURL* url=[NSURL fileURLWithPath:[NSString stringWithUTF8String:temporary]];
        if (!url) {
            std::fprintf(stderr,"[APPLE_CAPTURE_FAILED] invalid_path\n");return;
        }
        destination=CGImageDestinationCreateWithURL((CFURLRef)url,CFSTR("public.png"),1,nullptr);
        if(destination) {
            CGImageDestinationAddImage(destination,bitmap,nullptr);
            const bool finalized=CGImageDestinationFinalize(destination);
            CFRelease(destination);destination=nullptr;
            saved=finalized && std::rename(temporary,path)==0;
        }
        std::fprintf(stderr,"[APPLE_CAPTURE] saved=%d path=%s extent=%lux%lu performanceRun=0\n",
            saved,path,width,height);
        } @catch (NSException* exception) {
            // Capturing is optional diagnostics; an Objective-C allocation
            // exception must not terminate the game's presentation thread.
            std::fprintf(stderr,"[APPLE_CAPTURE_FAILED] objc_exception class=%s\n",
                object_getClassName(exception));
        } @finally {
        if(destination) CFRelease(destination);
        if(bitmap) CGImageRelease(bitmap);
        if(space) CGColorSpaceRelease(space);
        if(provider) CGDataProviderRelease(provider);
        [pixels release];[queue release];
        if (!saved && temporary[0]) std::remove(temporary);
        }
    }
}
