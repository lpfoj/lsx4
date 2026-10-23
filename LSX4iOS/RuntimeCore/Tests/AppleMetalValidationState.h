// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#import <Metal/Metal.h>
#include <cstdio>
#include <cstdlib>

// A requested environment setting is not evidence that shader instrumentation
// was activated. Query a pipeline using the default policy, without forcing its
// descriptor to opt in. This is diagnostic-only and never submits GPU work.
extern "C" void LSX4CheckAppleMetalValidationState() {
    const char* requested=std::getenv("MTL_SHADER_VALIDATION");
    if(!requested || std::strtol(requested,nullptr,10)==0) return;
    @autoreleasepool {
        if(@available(iOS 18.0,macOS 15.0,*)) {
            id<MTLDevice> device=MTLCreateSystemDefaultDevice();
            NSError* error=nil;
            id<MTLLibrary> library=[device newLibraryWithSource:
                @"#include <metal_stdlib>\nusing namespace metal;\n"
                 "kernel void lsx4_validation_probe(device uint* output [[buffer(0)]], "
                 "uint tid [[thread_position_in_grid]]) { output[tid]=tid; }"
                options:nil error:&error];
            id<MTLFunction> function=[library newFunctionWithName:@"lsx4_validation_probe"];
            id<MTLComputePipelineState> pipeline=function
                ? [device newComputePipelineStateWithFunction:function error:&error] : nil;
            const bool enabled=pipeline && pipeline.shaderValidation==MTLShaderValidationEnabled;
            std::fprintf(stderr,"[APPLE_METAL_VALIDATION_STATE] pipelineCreated=%d enabled=%d "
                "scope=default-compute-policy error=%s\n",pipeline!=nil,enabled,
                error?error.localizedDescription.UTF8String:"none");
            [pipeline release];[function release];[library release];[device release];
        } else {
            std::fprintf(stderr,"[APPLE_METAL_VALIDATION_STATE] enabled=unverified reason=query-unavailable\n");
        }
    }
}
