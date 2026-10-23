// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"

namespace Libraries::Kernel {
struct PthreadAttr;
}

namespace Core {

using ThreadFunc = void (*)(void*);
using PthreadFunc = void* (*)(void*);

class NativeThread {
public:
    NativeThread();
    ~NativeThread();

    int Create(ThreadFunc func, void* arg, const ::Libraries::Kernel::PthreadAttr* attr);
    int CreateDefaultStack(ThreadFunc func, void* arg);
#if defined(__ANDROID__) || defined(LSX4_APPLE_RUNTIME)
    // Native Android helpers must not let bionic place pthread_internal_t in a
    // guest stack whose storage is owned and recycled by the emulated kernel.
    int CreateDetachedDefaultStack(ThreadFunc func, void* arg);
#endif
    void Exit();
    void CleanupBeforeReturn();

    void Initialize();

    uintptr_t GetHandle() {
        return reinterpret_cast<uintptr_t>(native_handle);
    }

    u64 GetTid() {
        return tid;
    }

private:
#ifdef _WIN64
    void* native_handle;
#else
    uintptr_t native_handle;
    void* sig_stack_ptr;
#endif
    u64 tid;
};

}
