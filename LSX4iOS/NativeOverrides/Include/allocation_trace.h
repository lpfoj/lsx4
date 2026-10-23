// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdio>
#include <cstdlib>
#include <cstring>
namespace Lsx4::Diagnostics {
// Allocation statistics/stack walks are diagnostic work, never required for
// GPU memory admission, stream protection, barriers or readback correctness.
inline bool AllocationTraceEnabled() {
    static const bool enabled=[] {
        const auto* value=std::getenv("EXECUTOR_TRACE_GPU_ALLOCATIONS");
        const bool trace=value && std::strcmp(value,"1")==0;
        const auto* legacy=std::getenv("LSX4_APPLE_TLOU_MEMORY_DIAGNOSTIC");
        std::fprintf(stderr,"[EXECUTOR_ALLOCATION_TRACE] enabled=%d legacyMemoryDiagnostic=%s memoryOperationsUnchanged=1\n",
            trace,legacy ? legacy : "unset");
        return trace;
    }();
    return enabled;
}
}
