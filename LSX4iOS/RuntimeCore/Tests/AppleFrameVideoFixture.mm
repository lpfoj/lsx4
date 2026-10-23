// SPDX-License-Identifier: GPL-2.0-or-later
// Synthetic Metal frames: no guest assets, independent decoder validates RGB.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <vector>
#include <cstdio>
namespace Lsx4::Apple { void RequestDiagnosticFrameCapture(void (*)(void*) noexcept) noexcept {} }
extern "C" void vkGetMTLTextureMVK(void* image,id<MTLTexture>* texture) { *texture=(id<MTLTexture>)image; }
#include "AppleFrameVideo.h"
int main(int argc,char** argv) {
 if(argc<2)return 64;
 const bool interrupted=argc>2 && std::strcmp(argv[2],"interrupt")==0;
 const bool cancelled=argc>2 && std::strcmp(argv[2],"cancel")==0;
 if(interrupted)setenv("LSX4_PROBE_VIDEO_FRAGMENTS","1",1);
 else unsetenv("LSX4_PROBE_VIDEO_FRAGMENTS");
 @autoreleasepool {
  NSString* path=@(argv[1]);[NSFileManager.defaultManager createDirectoryAtPath:path withIntermediateDirectories:YES attributes:nil error:nil];
  AppleFrameVideo::state.directory=[path copy];AppleFrameVideo::state.active=true;
  if(argc==2 || interrupted || cancelled) {
   id<MTLDevice> device=MTLCreateSystemDefaultDevice();
   MTLTextureDescriptor* desc=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:128 height:96 mipmapped:NO];desc.storageMode=MTLStorageModeShared;
   id<MTLTexture> texture=[device newTextureWithDescriptor:desc];
   std::vector<unsigned char> bytes(128*96*4);
   for(unsigned n=0;n<(interrupted?160u:18u);++n) {
    for(unsigned y=0;y<96;++y)for(unsigned x=0;x<128;++x) {
     auto i=(y*128+x)*4;bytes[i]=n<6?0:255;bytes[i+1]=n>=12?255:0;bytes[i+2]=n<6?255:0;bytes[i+3]=255;
    }
    [texture replaceRegion:MTLRegionMake2D(0,0,128,96) mipmapLevel:0 withBytes:bytes.data() bytesPerRow:128*4];
    if(cancelled && n==2)[AppleFrameVideo::state.writer cancelWriting];
    AppleFrameVideo::Capture((void*)texture);[NSThread sleepForTimeInterval:.04];
    if(cancelled && n==2 && AppleFrameVideo::state.active)return 5;
   }
   [texture release];[device release];
  }
  if(interrupted)std::_Exit(17); // Simulate guest/process crash without finalization.
  AppleFrameVideo::Finish();
  NSString* report=[path stringByAppendingPathComponent:@"recording.json"];
  for(int i=0;i<100 && ![NSFileManager.defaultManager fileExistsAtPath:report];++i)[NSThread sleepForTimeInterval:.05];
  NSData* data=[NSData dataWithContentsOfFile:report];if(!data)return 2;
  NSDictionary* obj=[NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
  std::printf("%s\n",[[[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding] UTF8String]);
  if(cancelled)return [obj[@"status"] isEqual:@"failed"] && [obj[@"writerStatus"] intValue]==AVAssetWriterStatusCancelled && obj[@"failureReason"] ?0:6;
  return argc==2 ? ([obj[@"frames"] intValue]>=16 && [obj[@"status"] isEqual:@"completed"] ?0:3) : ([obj[@"status"] isEqual:@"no_frames"]?0:4);
 }
}
