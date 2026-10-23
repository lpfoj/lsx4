// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
// Standalone native-Metal control experiment: conditional discard must preserve
// stencil semantics with no fragment output, an unused color target, and a
// color-producing comparison. Readback checks are independent of the emulator.
// No title code/data, patch, or production render behavior is used here.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <cstdio>
#include <cstring>
#include <vector>
extern "C" int LSX4RunAppleEmptyFragmentFixture() { @autoreleasepool {
 id<MTLDevice> device=MTLCreateSystemDefaultDevice();
 NSError* error=nil;
 NSString* source=@"#include <metal_stdlib>\nusing namespace metal;\n"
 "struct In { float4 p [[attribute(0)]]; float4 c [[attribute(1)]]; };\n"
 "struct Out { float4 p [[position]]; float4 c [[user(locn0)]]; };\n"
 "struct Buf { uint data[1]; };\n"
 "vertex Out vert(In v [[stage_in]]) { return {v.p,v.c}; }\n"
 "fragment void no_color(Out v [[stage_in]], const device Buf& b [[buffer(1)]]) {\n"
 "bool enabled=b.data[8]!=0; bool accept; bool branch;\n"
 "if(enabled) { bool test=!(as_type<float>(b.data[9])>=v.c.w); accept=test; branch=test; }\n"
 "else { accept=true; branch=enabled; }\n"
 "if((!enabled)||branch) { if(b.data[0]!=0) {} if(!accept) discard_fragment(); return; }\n"
 "discard_fragment(); }\n"
 "fragment float4 with_color(Out v [[stage_in]], const device Buf& b [[buffer(1)]]) {\n"
 "if(b.data[8]!=0 && as_type<float>(b.data[9])>=v.c.w) discard_fragment(); return float4(1,0,0,1); }\n"
 "fragment float4 show(Out v [[stage_in]]) { return float4(1,0,0,1); }\n";
 id<MTLLibrary> lib=[device newLibraryWithSource:source options:nil error:&error];
 if(!lib){fprintf(stderr,"library: %s\n",error.description.UTF8String);return 1;}
 auto desc=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:64 height:64 mipmapped:NO];
 desc.usage=MTLTextureUsageRenderTarget;desc.storageMode=MTLStorageModeShared;
 id<MTLTexture> color=[device newTextureWithDescriptor:desc];
 desc.pixelFormat=MTLPixelFormatDepth32Float_Stencil8;desc.storageMode=MTLStorageModePrivate;
 id<MTLTexture> depth=[device newTextureWithDescriptor:desc];
 const float vertices[]={-1,-1,0.5,1,1,1,1,0, 1,-1,0.5,1,1,1,1,1, -1,1,0.5,1,1,1,1,0, 1,1,0.5,1,1,1,1,1};
 const uint32_t indices[]={0,1,2,3};uint32_t params[128]={};params[8]=1;float threshold=.5;memcpy(params+9,&threshold,4);
 id<MTLBuffer> vb=[device newBufferWithBytes:vertices length:sizeof(vertices) options:MTLResourceStorageModeShared];
 id<MTLBuffer> ib=[device newBufferWithBytes:indices length:sizeof(indices) options:MTLResourceStorageModeShared];
 id<MTLBuffer> uniforms=[device newBufferWithBytes:params length:sizeof(params) options:MTLResourceStorageModeShared];
 id<MTLCommandQueue> queue=[device newCommandQueue];
 auto layout=[MTLVertexDescriptor vertexDescriptor];layout.attributes[0].format=MTLVertexFormatFloat4;layout.attributes[0].bufferIndex=2;layout.attributes[1].format=MTLVertexFormatFloat4;layout.attributes[1].offset=16;layout.attributes[1].bufferIndex=2;layout.layouts[2].stride=32;
 auto pipeline=[MTLRenderPipelineDescriptor new];pipeline.vertexFunction=[lib newFunctionWithName:@"vert"];pipeline.vertexDescriptor=layout;pipeline.depthAttachmentPixelFormat=MTLPixelFormatDepth32Float_Stencil8;pipeline.stencilAttachmentPixelFormat=MTLPixelFormatDepth32Float_Stencil8;
 auto ds=[MTLDepthStencilDescriptor new];ds.depthCompareFunction=MTLCompareFunctionAlways;ds.depthWriteEnabled=YES;
 auto stencil=[MTLStencilDescriptor new];stencil.stencilCompareFunction=MTLCompareFunctionAlways;stencil.depthStencilPassOperation=MTLStencilOperationReplace;ds.frontFaceStencil=stencil;ds.backFaceStencil=stencil;
 id<MTLDepthStencilState> mark=[device newDepthStencilStateWithDescriptor:ds];
 stencil.stencilCompareFunction=MTLCompareFunctionEqual;stencil.depthStencilPassOperation=MTLStencilOperationKeep;ds.depthWriteEnabled=NO;ds.frontFaceStencil=stencil;ds.backFaceStencil=stencil;
 id<MTLDepthStencilState> check=[device newDepthStencilStateWithDescriptor:ds];
 pipeline.fragmentFunction=[lib newFunctionWithName:@"show"];pipeline.colorAttachments[0].pixelFormat=MTLPixelFormatRGBA8Unorm;
 id<MTLRenderPipelineState> show=[device newRenderPipelineStateWithDescriptor:pipeline error:&error];
 if(!show){fprintf(stderr,"show pipeline: %s\n",error.description.UTF8String);return 2;}
 for(int variant=0;variant<5;variant++){
  bool attached=variant==1 || variant==2;pipeline.fragmentFunction=[lib newFunctionWithName:variant==2?@"with_color":@"no_color"];
  pipeline.depthAttachmentPixelFormat=variant>=3?MTLPixelFormatInvalid:MTLPixelFormatDepth32Float_Stencil8;pipeline.stencilAttachmentPixelFormat=pipeline.depthAttachmentPixelFormat;
  pipeline.colorAttachments[0].pixelFormat=attached?MTLPixelFormatRGBA8Unorm:MTLPixelFormatInvalid;pipeline.colorAttachments[0].writeMask=MTLColorWriteMaskNone;
  id<MTLRenderPipelineState> state=[device newRenderPipelineStateWithDescriptor:pipeline error:&error];
  if(!state){fprintf(stderr,"pipeline %d: %s\n",variant,error.description.UTF8String);return 3;}
  for(int repeat=0;repeat<16;repeat++){
   // Mixed, fully discarded and fully accepted primitives exercise both
   // shader exits, including an entire tile with no fragment/color output.
   float cutoff=(repeat%3==0)?.5f:(repeat%3==1)?2.f:-1.f;
   memcpy(static_cast<uint8_t*>(uniforms.contents)+9*sizeof(uint32_t),&cutoff,sizeof(cutoff));
   auto pass=[MTLRenderPassDescriptor renderPassDescriptor];pass.depthAttachment.texture=depth;pass.stencilAttachment.texture=depth;
   pass.depthAttachment.loadAction=MTLLoadActionClear;pass.depthAttachment.clearDepth=1;pass.depthAttachment.storeAction=MTLStoreActionStore;
   pass.stencilAttachment.loadAction=MTLLoadActionClear;pass.stencilAttachment.clearStencil=0;pass.stencilAttachment.storeAction=MTLStoreActionStore;
   if(attached){pass.colorAttachments[0].texture=color;pass.colorAttachments[0].loadAction=MTLLoadActionClear;pass.colorAttachments[0].storeAction=MTLStoreActionStore;}
   if(variant>=3){
    pass.depthAttachment.texture=nil;pass.stencilAttachment.texture=nil;
    // Contrast a normal game extent with the largest attachmentless extent.
    const NSUInteger side=variant==3?2048:([device supportsFamily:MTLGPUFamilyApple10]?32768:16384);
    pass.renderTargetWidth=side;pass.renderTargetHeight=side;pass.defaultRasterSampleCount=1;
   }
   auto cb=[queue commandBuffer];auto enc=[cb renderCommandEncoderWithDescriptor:pass];
   [enc setRenderPipelineState:state];[enc setDepthStencilState:mark];[enc setStencilReferenceValue:0x5a];[enc setVertexBuffer:vb offset:0 atIndex:2];[enc setFragmentBuffer:uniforms offset:0 atIndex:1];
   if(variant>=3){[enc setViewport:MTLViewport{0,0,1920,1080,0,1}];[enc setScissorRect:MTLScissorRect{0,0,1920,1080}];}
   [enc drawIndexedPrimitives:MTLPrimitiveTypeTriangleStrip indexCount:4 indexType:MTLIndexTypeUInt32 indexBuffer:ib indexBufferOffset:0];[enc endEncoding];
   if(variant>=3){
    [cb commit];[cb waitUntilCompleted];
    if(cb.status!=MTLCommandBufferStatusCompleted){fprintf(stderr,"ATTACHMENTLESS failed variant=%d: %s\n",variant,cb.error.description.UTF8String);return 6;}
    continue;
   }
   pass.depthAttachment.loadAction=MTLLoadActionLoad;pass.stencilAttachment.loadAction=MTLLoadActionLoad;pass.colorAttachments[0].texture=color;pass.colorAttachments[0].loadAction=MTLLoadActionClear;pass.colorAttachments[0].storeAction=MTLStoreActionStore;
   enc=[cb renderCommandEncoderWithDescriptor:pass];[enc setRenderPipelineState:show];[enc setDepthStencilState:check];[enc setStencilReferenceValue:0x5a];[enc setVertexBuffer:vb offset:0 atIndex:2];
   [enc drawIndexedPrimitives:MTLPrimitiveTypeTriangleStrip indexCount:4 indexType:MTLIndexTypeUInt32 indexBuffer:ib indexBufferOffset:0];[enc endEncoding];[cb commit];[cb waitUntilCompleted];
   if(cb.status!=MTLCommandBufferStatusCompleted){fprintf(stderr,"GPU failed variant=%d repeat=%d: %s\n",variant,repeat,cb.error.description.UTF8String);return 4;}
   std::vector<uint8_t> pixels(64*64*4);[color getBytes:pixels.data() bytesPerRow:256 fromRegion:MTLRegionMake2D(0,0,64,64) mipmapLevel:0];
   if(pixels[(32*64+8)*4]!=(repeat%3==2?255:0) || pixels[(32*64+56)*4]!=(repeat%3==1?0:255)){fprintf(stderr,"stencil mismatch variant=%d left=%u right=%u\n",variant,pixels[(32*64+8)*4],pixels[(32*64+56)*4]);return 5;}
  }
  fprintf(stderr,"EMPTY_FRAGMENT variant=%d repetitions=16 %s=passed\n",variant,variant>=3?"attachmentlessCompletion":"stencilReadback");[state release];
 }
 [show release];[check release];[mark release];[stencil release];[ds release];
 [pipeline release];[queue release];[uniforms release];[ib release];[vb release];
 [depth release];[color release];[lib release];[device release];
 return 0;
}}
