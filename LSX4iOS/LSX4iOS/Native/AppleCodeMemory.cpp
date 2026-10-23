// SPDX-License-Identifier: GPL-2.0-or-later
#include "AppleCodeMemory.h"
#include <TargetConditionals.h>
#include <mach/mach.h>
#include <libkern/OSCacheControl.h>
#include <sys/mman.h>
#include <sys/sysctl.h>
#include <unistd.h>
#include <cstring>
#include <cerrno>
#include <cstdlib>

// Xcode's LLDB helper stops here and touches the RX pages using its existing
// debug connection. iOS 26 requires this before code written via the RW alias
// is executable. The acknowledgement prevents execution with a missing helper.

#if defined(LSX4_DEVICE_LOCAL_JIT)
extern "C" __attribute__((noinline,optnone,naked,used))
void LSX4LocalPrepareRegion(void*, std::size_t) {
    asm volatile("mov x16, #1\n brk #0xf00d\n ret\n");
}
extern "C" __attribute__((noinline,optnone,naked,used))
void LSX4LocalDetachDebugger() {
    asm volatile("mov x16, #0\n brk #0xf00d\n ret\n");
}
#endif
extern "C" __attribute__((noinline, used, visibility("default")))
void LSX4DebugPrepareCodePages(void* address, std::size_t size) {
#if defined(LSX4_DEVICE_LOCAL_JIT)
    if (const char* provider = std::getenv("LSX4_JIT_PROVIDER");
        provider && std::strcmp(provider, "local") == 0) {
        LSX4LocalPrepareRegion(address, size);
        return;
    }
#endif
    asm volatile("" : : "r"(address), "r"(size) : "memory");
}

namespace Lsx4::Apple {
CodeMemory::~CodeMemory() {
    if (writable_) vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(writable_), size_);
    if (executable_) munmap(executable_, size_);
}

bool CodeMemory::Allocate(std::size_t bytes, std::string& error) {
#if !DEBUG || TARGET_OS_SIMULATOR
    error = "Device JIT requires a Debug build launched from Xcode.";
    return false;
#else
    if (executable_ || !bytes || bytes > 4 * 1024 * 1024) {
        error = "Invalid code allocation size or repeated allocation.";
        return false;
    }
    int mib[] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid()};
    struct kinfo_proc process{};
    size_t infoSize = sizeof(process);
    if (sysctl(mib, 4, &process, &infoSize, nullptr, 0) || !(process.kp_proc.p_flag & P_TRACED)) {
        error = "Xcode debugger is not attached.";
        return false;
    }
    const std::size_t page = getpagesize();
    size_ = (bytes + page - 1) / page * page;
    executable_ = mmap(nullptr, size_, PROT_READ | PROT_EXEC, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (executable_ == MAP_FAILED) {
        executable_ = nullptr;
        error = strerror(errno);
        return false;
    }
    vm_address_t alias = 0;
    vm_prot_t current = 0, maximum = 0;
    const auto result = vm_remap(mach_task_self(), &alias, size_, page - 1,
        VM_FLAGS_ANYWHERE, mach_task_self(), reinterpret_cast<vm_address_t>(executable_),
        FALSE, &current, &maximum, VM_INHERIT_NONE);
    if (result != KERN_SUCCESS) {
        error = mach_error_string(result);
        return false;
    }
    writable_ = reinterpret_cast<void*>(alias);
    LSX4DebugPrepareCodePages(executable_, size_);
    if (std::memcmp(executable_, "LSX4JIT!", 8) != 0) {
        error = "Load LSX4iOS/Debug/lsx4.lldbinit in the Xcode Run scheme and restart.";
        return false;
    }
    if (mprotect(writable_, size_, PROT_READ | PROT_WRITE)) {
        error = strerror(errno);
        return false;
    }
    return true;
#endif
}

bool CodeMemory::Publish(std::size_t bytes, std::string& error) {
    if (!writable_ || !executable_ || !bytes || bytes > size_) {
        error = "Invalid code publication.";
        return false;
    }
    if (std::memcmp(executable_, writable_, bytes) != 0) {
        error = "Code aliases do not share bytes.";
        return false;
    }
    sys_dcache_flush(writable_, bytes);
    sys_icache_invalidate(const_cast<void*>(executable_), bytes);
    return true;
}
}
