// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <bit>
#ifdef __ANDROID__
#include <android/log.h>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <limits>
#include <map>
#include <mutex>
#include <sys/mman.h>
#include <vector>
#endif

#include "common/alignment.h"
#include "common/assert.h"
#include "common/elf_info.h"
#include "common/logging/log.h"
#include "common/scope_exit.h"
#include "common/singleton.h"
#include "core/libraries/kernel/kernel.h"
#include "core/libraries/kernel/memory.h"
#include "core/libraries/kernel/orbis_error.h"
#include "core/libraries/kernel/process.h"
#include "core/libraries/libs.h"
#include "core/linker.h"
#include "core/memory.h"

namespace Libraries::Kernel {

static s32 g_sdk_version = -1;
static bool g_alias_dmem = false;

#ifdef __ANDROID__
static bool ExecutorHostMmapContains(VAddr addr);
static int ExecutorToHostProt(Core::MemoryProt prot);
static s32 ExecutorHostMprotect(VAddr addr, u64 size, Core::MemoryProt prot);
static void* ExecutorHostMmap(VAddr requested_addr, u64 size, Core::MemoryProt prot,
                              bool fixed);
static s32 ExecutorHostMunmap(VAddr addr, u64 size);

static void ExecutorLiveMmapLog(const char* event, const void* in_addr, u64 len, s32 prot,
                                s32 flags, s32 fd, s64 phys_addr, const void* out_addr,
                                int err, s32 result) {
    __android_log_print(ANDROID_LOG_INFO, "LSX4Native",
                        "[EXECUTOR_LIVE_MMAP] event=%s in=%p len=0x%llx prot=0x%x flags=0x%x "
                        "fd=%d phys=0x%llx out=%p errno=%d result=0x%x",
                        event, in_addr, static_cast<unsigned long long>(len), prot, flags, fd,
                        static_cast<unsigned long long>(phys_addr), out_addr, err, result);
}
#endif

u64 PS4_SYSV_ABI sceKernelGetDirectMemorySize() {
    LOG_TRACE(Kernel_Vmm, "called");
    const auto* memory = Core::Memory::Instance();
    return memory->GetTotalDirectSize();
}

s32 PS4_SYSV_ABI sceKernelEnableDmemAliasing() {
    LOG_DEBUG(Kernel_Vmm, "called");
    g_alias_dmem = true;
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceKernelAllocateDirectMemory(s64 searchStart, s64 searchEnd, u64 len,
                                               u64 alignment, s32 memoryType, s64* physAddrOut) {
#ifdef __ANDROID__
    if (len == 0x10000000ull &&
        access("/data/data/app.lsx4.android/files/lsx4-home/diag-fail-directmem", F_OK) ==
            0) {
        __android_log_print(ANDROID_LOG_INFO, "LSX4Native",
                            "[EXECUTOR_DIAG_FAIL_DIRECTMEM] len=0x%llx -> forced EAGAIN",
                            static_cast<unsigned long long>(len));
        return ORBIS_KERNEL_ERROR_EAGAIN;
    }
#endif
    if (searchStart < 0 || searchEnd < 0) {
        LOG_ERROR(Kernel_Vmm, "Invalid parameters!");
        return ORBIS_KERNEL_ERROR_EINVAL;
    }
    if (len <= 0 || !Common::Is16KBAligned(len)) {
        LOG_ERROR(Kernel_Vmm, "Length {:#x} is invalid!", len);
        return ORBIS_KERNEL_ERROR_EINVAL;
    }
    if (alignment != 0 && !Common::Is16KBAligned(alignment)) {
        LOG_ERROR(Kernel_Vmm, "Alignment {:#x} is invalid!", alignment);
        return ORBIS_KERNEL_ERROR_EINVAL;
    }
    if (memoryType > 10) {
        LOG_ERROR(Kernel_Vmm, "Memory type {:#x} is invalid!", memoryType);
        return ORBIS_KERNEL_ERROR_EINVAL;
    }
    if (physAddrOut == nullptr) {
        LOG_ERROR(Kernel_Vmm, "Result physical address pointer is null!");
        return ORBIS_KERNEL_ERROR_EINVAL;
    }

    const bool is_in_range = searchEnd - searchStart >= len;
    if (searchEnd <= searchStart || searchEnd < len || !is_in_range) {
        LOG_ERROR(Kernel_Vmm,
                  "Provided address range is too small!"
                  " searchStart = {:#x}, searchEnd = {:#x}, length = {:#x}",
                  searchStart, searchEnd, len);
        return ORBIS_KERNEL_ERROR_EAGAIN;
    }

    auto* memory = Core::Memory::Instance();
    PAddr phys_addr = memory->Allocate(searchStart, searchEnd, len, alignment, memoryType);
    if (phys_addr == -1) {
#ifdef __ANDROID__
        __android_log_print(
            ANDROID_LOG_ERROR, "LSX4Native",
            "[EXECUTOR_DMEM_ALLOC_FAIL] searchStart=0x%llx searchEnd=0x%llx "
            "len=0x%llx alignment=0x%llx memoryType=0x%x physOutPtr=%p result=0x%x",
            static_cast<unsigned long long>(searchStart),
            static_cast<unsigned long long>(searchEnd),
            static_cast<unsigned long long>(len),
            static_cast<unsigned long long>(alignment), memoryType,
            static_cast<void*>(physAddrOut),
            static_cast<unsigned>(ORBIS_KERNEL_ERROR_EAGAIN));
#endif
        return ORBIS_KERNEL_ERROR_EAGAIN;
    }

    *physAddrOut = static_cast<s64>(phys_addr);

    LOG_INFO(Kernel_Vmm,
             "searchStart = {:#x}, searchEnd = {:#x}, len = {:#x}, "
             "alignment = {:#x}, memoryType = {:#x}, physAddrOut = {:#x}",
             searchStart, searchEnd, len, alignment, memoryType, phys_addr);
#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_INFO, "LSX4Native",
                        "[EXECUTOR_DMEM_ALLOC] searchStart=0x%llx searchEnd=0x%llx len=0x%llx "
                        "alignment=0x%llx memoryType=0x%x physOutPtr=%p phys=0x%llx",
                        static_cast<unsigned long long>(searchStart),
                        static_cast<unsigned long long>(searchEnd),
                        static_cast<unsigned long long>(len),
                        static_cast<unsigned long long>(alignment), memoryType,
                        static_cast<void*>(physAddrOut), static_cast<unsigned long long>(phys_addr));
#endif

    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceKernelAllocateMainDirectMemory(u64 len, u64 alignment, s32 memoryType,
                                                   s64* physAddrOut) {
    const auto searchEnd = static_cast<s64>(sceKernelGetDirectMemorySize());
    return sceKernelAllocateDirectMemory(0, searchEnd, len, alignment, memoryType, physAddrOut);
}

s32 PS4_SYSV_ABI sceKernelCheckedReleaseDirectMemory(u64 start, u64 len) {
    LOG_INFO(Kernel_Vmm, "called start = {:#x}, len = {:#x}", start, len);
    if (!Common::Is16KBAligned(start) || !Common::Is16KBAligned(len)) {
        LOG_ERROR(Kernel_Vmm, "Misaligned start or length, start = {:#x}, length = {:#x}", start,
                  len);
        return ORBIS_KERNEL_ERROR_EINVAL;
    }
    if (len == 0) {
        return ORBIS_OK;
    }
    auto* memory = Core::Memory::Instance();
    const s32 result = memory->Free(start, len, true);
#ifdef __ANDROID__
    static std::atomic<u64> release_count{};
    const u64 count = release_count.fetch_add(1, std::memory_order_relaxed) + 1;
    if (count <= 32 || std::has_single_bit(count)) {
        __android_log_print(
            ANDROID_LOG_INFO, "LSX4Native",
            "[EXECUTOR_DMEM_RELEASE] checked=1 count=%llu start=0x%llx len=0x%llx result=0x%x",
            static_cast<unsigned long long>(count), static_cast<unsigned long long>(start),
            static_cast<unsigned long long>(len), static_cast<unsigned>(result));
    }
#endif
    return result;
}

s32 PS4_SYSV_ABI sceKernelReleaseDirectMemory(u64 start, u64 len) {
    LOG_INFO(Kernel_Vmm, "called start = {:#x}, len = {:#x}", start, len);
    if (!Common::Is16KBAligned(start) || !Common::Is16KBAligned(len)) {
        LOG_ERROR(Kernel_Vmm, "Misaligned start or length, start = {:#x}, length = {:#x}", start,
                  len);
        return ORBIS_KERNEL_ERROR_EINVAL;
    }
    if (len == 0) {
        return ORBIS_OK;
    }
    auto* memory = Core::Memory::Instance();
    const s32 result = memory->Free(start, len, false);
#ifdef __ANDROID__
    static std::atomic<u64> release_count{};
    const u64 count = release_count.fetch_add(1, std::memory_order_relaxed) + 1;
    if (count <= 32 || std::has_single_bit(count)) {
        __android_log_print(
            ANDROID_LOG_INFO, "LSX4Native",
            "[EXECUTOR_DMEM_RELEASE] checked=0 count=%llu start=0x%llx len=0x%llx result=0x%x",
            static_cast<unsigned long long>(count), static_cast<unsigned long long>(start),
            static_cast<unsigned long long>(len), static_cast<unsigned>(result));
    }
#endif
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceKernelAvailableDirectMemorySize(u64 searchStart, u64 searchEnd, u64 alignment,
                                                    u64* physAddrOut, u64* sizeOut) {
    LOG_INFO(Kernel_Vmm, "called searchStart = {:#x}, searchEnd = {:#x}, alignment = {:#x}",
             searchStart, searchEnd, alignment);

    if (physAddrOut == nullptr || sizeOut == nullptr) {
        return ORBIS_KERNEL_ERROR_EINVAL;
    }

    auto* memory = Core::Memory::Instance();

    PAddr physAddr{};
    u64 size{};
    s32 result = memory->DirectQueryAvailable(searchStart, searchEnd, alignment, &physAddr, &size);

    if (size == 0) {
        return ORBIS_KERNEL_ERROR_ENOMEM;
    }

    *physAddrOut = static_cast<u64>(physAddr);
    *sizeOut = size;

    return result;
}

s32 PS4_SYSV_ABI sceKernelVirtualQuery(const void* addr, s32 flags, OrbisVirtualQueryInfo* info,
                                       u64 infoSize) {
    LOG_INFO(Kernel_Vmm, "called addr = {}, flags = {:#x}", fmt::ptr(addr), flags);
    auto* memory = Core::Memory::Instance();
    return memory->VirtualQuery(std::bit_cast<VAddr>(addr), flags, info);
}

s32 PS4_SYSV_ABI sceKernelReserveVirtualRange(void** addr, u64 len, s32 flags, u64 alignment) {
    LOG_INFO(Kernel_Vmm, "addr = {}, len = {:#x}, flags = {:#x}, alignment = {:#x}",
             fmt::ptr(*addr), len, flags, alignment);
    if (addr == nullptr) {
        LOG_ERROR(Kernel_Vmm, "Address is invalid!");
        return ORBIS_KERNEL_ERROR_EINVAL;
    }
    if (len == 0 || !Common::Is16KBAligned(len)) {
        LOG_ERROR(Kernel_Vmm, "Map size is either zero or not 16KB aligned!");
        return ORBIS_KERNEL_ERROR_EINVAL;
    }
    if (alignment != 0) {
        if ((!std::has_single_bit(alignment) && !Common::Is16KBAligned(alignment))) {
            LOG_ERROR(Kernel_Vmm, "Alignment value is invalid!");
            return ORBIS_KERNEL_ERROR_EINVAL;
        }
    }

    auto* memory = Core::Memory::Instance();
    const VAddr in_addr = reinterpret_cast<VAddr>(*addr);
    const auto map_flags = static_cast<Core::MemoryMapFlags>(flags);

    s32 result = memory->MapMemory(addr, in_addr, len, Core::MemoryProt::NoAccess, map_flags,
                                   Core::VMAType::Reserved, "anon", false, -1, alignment);
    if (result == 0) {
        LOG_INFO(Kernel_Vmm, "out_addr = {}", fmt::ptr(*addr));
    }
    return result;
}

s32 PS4_SYSV_ABI sceKernelMapNamedDirectMemory(void** addr, u64 len, s32 prot, s32 flags,
                                               s64 phys_addr, u64 alignment, const char* name) {
    LOG_INFO(Kernel_Vmm,
             "in_addr = {}, len = {:#x}, prot = {:#x}, flags = {:#x}, "
             "phys_addr = {:#x}, alignment = {:#x}, name = '{}'",
             fmt::ptr(*addr), len, prot, flags, phys_addr, alignment, name);

    if (len == 0 || !Common::Is16KBAligned(len)) {
        LOG_ERROR(Kernel_Vmm, "Map size is either zero or not 16KB aligned!");
        return ORBIS_KERNEL_ERROR_EINVAL;
    }

    if (!Common::Is16KBAligned(phys_addr)) {
        LOG_ERROR(Kernel_Vmm, "Start address is not 16KB aligned!");
        return ORBIS_KERNEL_ERROR_EINVAL;
    }

    if (alignment != 0) {
        if ((!std::has_single_bit(alignment) && !Common::Is16KBAligned(alignment))) {
            LOG_ERROR(Kernel_Vmm, "Alignment value is invalid!");
            return ORBIS_KERNEL_ERROR_EINVAL;
        }
    }

    if (std::strlen(name) >= ORBIS_KERNEL_MAXIMUM_NAME_LENGTH) {
        LOG_ERROR(Kernel_Vmm, "name exceeds 32 bytes!");
        return ORBIS_KERNEL_ERROR_ENAMETOOLONG;
    }

    const auto mem_prot = static_cast<Core::MemoryProt>(prot);
    if (True(mem_prot & Core::MemoryProt::CpuExec)) {
        LOG_ERROR(Kernel_Vmm, "Executable permissions are not allowed.");
        return ORBIS_KERNEL_ERROR_EACCES;
    }

    const auto map_flags = static_cast<Core::MemoryMapFlags>(flags);
    const VAddr in_addr = reinterpret_cast<VAddr>(*addr);

    auto* memory = Core::Memory::Instance();
    bool should_check = false;
    if (g_sdk_version >= Common::ElfInfo::FW_25 && False(map_flags & Core::MemoryMapFlags::Stack)) {
        should_check = !g_alias_dmem;
    }
    const auto ret =
        memory->MapMemory(addr, in_addr, len, mem_prot, map_flags, Core::VMAType::Direct, name,
                          should_check, phys_addr, alignment);

    LOG_INFO(Kernel_Vmm, "out_addr = {}", fmt::ptr(*addr));
#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_INFO, "LSX4Native",
                        "[EXECUTOR_DMEM_MAP] name=%s in=0x%llx len=0x%llx prot=0x%x flags=0x%x "
                        "phys=0x%llx alignment=0x%llx ret=0x%x out=%p",
                        name ? name : "<null>", static_cast<unsigned long long>(in_addr),
                        static_cast<unsigned long long>(len), prot, flags,
                        static_cast<unsigned long long>(phys_addr),
                        static_cast<unsigned long long>(alignment), ret, *addr);
#endif
    return ret;
}

s32 PS4_SYSV_ABI sceKernelMapDirectMemory(void** addr, u64 len, s32 prot, s32 flags, s64 phys_addr,
                                          u64 alignment) {
    LOG_TRACE(Kernel_Vmm, "called, redirected to sceKernelMapNamedDirectMemory");
    return sceKernelMapNamedDirectMemory(addr, len, prot, flags, phys_addr, alignment, "anon");
}

s32 PS4_SYSV_ABI sceKernelMapDirectMemory2(void** addr, u64 len, s32 type, s32 prot, s32 flags,
                                           s64 phys_addr, u64 alignment) {
    LOG_INFO(Kernel_Vmm,
             "in_addr = {}, len = {:#x}, prot = {:#x}, flags = {:#x}, "
             "phys_addr = {:#x}, alignment = {:#x}",
             fmt::ptr(*addr), len, prot, flags, phys_addr, alignment);

    if (len == 0 || !Common::Is16KBAligned(len)) {
        LOG_ERROR(Kernel_Vmm, "Map size is either zero or not 16KB aligned!");
        return ORBIS_KERNEL_ERROR_EINVAL;
    }

    if (!Common::Is16KBAligned(phys_addr)) {
        LOG_ERROR(Kernel_Vmm, "Start address is not 16KB aligned!");
        return ORBIS_KERNEL_ERROR_EINVAL;
    }

    if (alignment != 0) {
        if ((!std::has_single_bit(alignment) && !Common::Is16KBAligned(alignment))) {
            LOG_ERROR(Kernel_Vmm, "Alignment value is invalid!");
            return ORBIS_KERNEL_ERROR_EINVAL;
        }
    }

    const auto mem_prot = static_cast<Core::MemoryProt>(prot);
    if (True(mem_prot & Core::MemoryProt::CpuExec)) {
        LOG_ERROR(Kernel_Vmm, "Executable permissions are not allowed.");
        return ORBIS_KERNEL_ERROR_EINVAL;
    }

    const auto map_flags = static_cast<Core::MemoryMapFlags>(flags);
    const VAddr in_addr = reinterpret_cast<VAddr>(*addr);

    auto* memory = Core::Memory::Instance();
    const auto ret =
        memory->MapMemory(addr, in_addr, len, mem_prot, map_flags, Core::VMAType::Direct, "anon",
                          !g_alias_dmem, phys_addr, alignment);

    if (ret == 0) {
        auto* memory = Core::Memory::Instance();
        const auto out_addr = reinterpret_cast<VAddr>(*addr);
        memory->SetDirectMemoryType(out_addr, len, type);
        LOG_INFO(Kernel_Vmm, "out_addr = {:#x}", out_addr);
    }
    return ret;
}

s32 PS4_SYSV_ABI sceKernelMapNamedFlexibleMemory(void** addr_in_out, u64 len, s32 prot, s32 flags,
                                                 const char* name) {
    LOG_INFO(Kernel_Vmm, "in_addr = {}, len = {:#x}, prot = {:#x}, flags = {:#x}, name = '{}'",
             fmt::ptr(*addr_in_out), len, prot, flags, name);
    if (len == 0 || !Common::Is16KBAligned(len)) {
        LOG_ERROR(Kernel_Vmm, "len is 0 or not 16kb multiple");
        return ORBIS_KERNEL_ERROR_EINVAL;
    }

    if (name == nullptr) {
        LOG_ERROR(Kernel_Vmm, "name is invalid!");
        return ORBIS_KERNEL_ERROR_EFAULT;
    }

    if (std::strlen(name) >= ORBIS_KERNEL_MAXIMUM_NAME_LENGTH) {
        LOG_ERROR(Kernel_Vmm, "name exceeds 32 bytes!");
        return ORBIS_KERNEL_ERROR_ENAMETOOLONG;
    }

    const VAddr in_addr = reinterpret_cast<VAddr>(*addr_in_out);
    const auto mem_prot = static_cast<Core::MemoryProt>(prot);
    const auto map_flags = static_cast<Core::MemoryMapFlags>(flags);
    auto* memory = Core::Memory::Instance();
    const auto ret = memory->MapMemory(addr_in_out, in_addr, len, mem_prot, map_flags,
                                       Core::VMAType::Flexible, name);
    LOG_INFO(Kernel_Vmm, "out_addr = {}", fmt::ptr(*addr_in_out));
    return ret;
}

s32 PS4_SYSV_ABI sceKernelMapFlexibleMemory(void** addr_in_out, u64 len, s32 prot, s32 flags) {
    return sceKernelMapNamedFlexibleMemory(addr_in_out, len, prot, flags, "anon");
}

s32 PS4_SYSV_ABI sceKernelQueryMemoryProtection(void* addr, void** start, void** end, u32* prot) {
    auto* memory = Core::Memory::Instance();
    return memory->QueryProtection(std::bit_cast<VAddr>(addr), start, end, prot);
}

s32 PS4_SYSV_ABI sceKernelMprotect(const void* addr, u64 size, s32 prot) {
    LOG_INFO(Kernel_Vmm, "called addr = {}, size = {:#x}, prot = {:#x}", fmt::ptr(addr), size,
             prot);

    if (size == 0) {
        return ORBIS_OK;
    }

    const VAddr in_addr = reinterpret_cast<VAddr>(addr);

#ifdef __ANDROID__
    if (ExecutorHostMmapContains(in_addr)) {
        constexpr VAddr HostPageSize = 4_KB;
        const VAddr host_aligned_addr = Common::AlignDown(in_addr, HostPageSize);
        if (size > std::numeric_limits<u64>::max() - (in_addr - host_aligned_addr)) {
            *__Error() = EINVAL;
            return ORBIS_KERNEL_ERROR_EINVAL;
        }
        const u64 host_aligned_size =
            Common::AlignUp(size + in_addr - host_aligned_addr, HostPageSize);
        const auto protection_flags = static_cast<Core::MemoryProt>(prot);
        return ExecutorHostMprotect(host_aligned_addr, host_aligned_size, protection_flags);
    }
#endif

    auto aligned_addr = Common::AlignDown(in_addr, 16_KB);
    auto aligned_size = Common::AlignUp(size + in_addr - aligned_addr, 16_KB);

    if (aligned_size == 0) {
        return ORBIS_OK;
    }

    Core::MemoryManager* memory_manager = Core::Memory::Instance();
    Core::MemoryProt protection_flags = static_cast<Core::MemoryProt>(prot);

    return memory_manager->Protect(aligned_addr, aligned_size, protection_flags);
}

s32 PS4_SYSV_ABI posix_mprotect(const void* addr, u64 size, s32 prot) {
    s32 result = sceKernelMprotect(addr, size, prot);
    if (result < 0) {
        ErrSceToPosix(result);
        return -1;
    }
    return result;
}

s32 PS4_SYSV_ABI sceKernelMtypeprotect(const void* addr, u64 size, s32 mtype, s32 prot) {
    LOG_INFO(Kernel_Vmm, "called addr = {}, size = {:#x}, prot = {:#x}", fmt::ptr(addr), size,
             prot);
    const VAddr in_addr = reinterpret_cast<VAddr>(addr);
    auto aligned_addr = Common::AlignDown(in_addr, 16_KB);
    auto aligned_size = Common::AlignUp(size + in_addr - aligned_addr, 16_KB);

    if (aligned_size == 0) {
        return ORBIS_OK;
    }

    Core::MemoryManager* memory_manager = Core::Memory::Instance();
    Core::MemoryProt protection_flags = static_cast<Core::MemoryProt>(prot);

    s32 result = memory_manager->Protect(aligned_addr, aligned_size, protection_flags);
    if (result == ORBIS_OK) {
        memory_manager->SetDirectMemoryType(aligned_addr, aligned_size, mtype);
    }
    return result;
}

s32 PS4_SYSV_ABI sceKernelDirectMemoryQuery(u64 offset, s32 flags, OrbisQueryInfo* query_info,
                                            u64 infoSize) {
    LOG_INFO(Kernel_Vmm, "called offset = {:#x}, flags = {:#x}", offset, flags);
    auto* memory = Core::Memory::Instance();
    return memory->DirectMemoryQuery(offset, flags == 1, query_info);
}

s32 PS4_SYSV_ABI sceKernelAvailableFlexibleMemorySize(u64* out_size) {
    auto* memory = Core::Memory::Instance();
    *out_size = memory->GetAvailableFlexibleSize();
#ifdef __ANDROID__
    if (*out_size < 0x20000000ull) {
        *out_size = 0x20000000ull;
    }
#endif
    LOG_INFO(Kernel_Vmm, "called size = {:#x}", *out_size);
    return ORBIS_OK;
}

void PS4_SYSV_ABI _sceKernelRtldSetApplicationHeapAPI(void* func[]) {
    auto* linker = Common::Singleton<Core::Linker>::Instance();
    linker->SetHeapAPI(func);
}

s32 PS4_SYSV_ABI sceKernelGetDirectMemoryType(u64 addr, s32* directMemoryTypeOut,
                                              void** directMemoryStartOut,
                                              void** directMemoryEndOut) {
    LOG_WARNING(Kernel_Vmm, "called, direct memory addr = {:#x}", addr);
    auto* memory = Core::Memory::Instance();
    return memory->GetDirectMemoryType(addr, directMemoryTypeOut, directMemoryStartOut,
                                       directMemoryEndOut);
}

s32 PS4_SYSV_ABI sceKernelIsStack(void* addr, void** start, void** end) {
    LOG_DEBUG(Kernel_Vmm, "called, addr = {}", fmt::ptr(addr));
    auto* memory = Core::Memory::Instance();
    return memory->IsStack(std::bit_cast<VAddr>(addr), start, end);
}

u32 PS4_SYSV_ABI sceKernelIsAddressSanitizerEnabled() {
    LOG_DEBUG(Kernel, "called");
    return false;
}

s32 PS4_SYSV_ABI sceKernelBatchMap(OrbisKernelBatchMapEntry* entries, s32 numEntries,
                                   s32* numEntriesOut) {
    return sceKernelBatchMap2(entries, numEntries, numEntriesOut,
                              MemoryFlags::ORBIS_KERNEL_MAP_FIXED);
}

s32 PS4_SYSV_ABI sceKernelBatchMap2(OrbisKernelBatchMapEntry* entries, s32 numEntries,
                                    s32* numEntriesOut, s32 flags) {
    s32 result = ORBIS_OK;
    s32 processed = 0;
    for (s32 i = 0; i < numEntries; i++, processed++) {
        if (entries == nullptr || entries[i].length == 0 || entries[i].operation > 4) {
            result = ORBIS_KERNEL_ERROR_EINVAL;
            break;
        }

        switch (entries[i].operation) {
        case MemoryOpTypes::ORBIS_KERNEL_MAP_OP_MAP_DIRECT: {
            result = sceKernelMapNamedDirectMemory(&entries[i].start, entries[i].length,
                                                   entries[i].protection, flags,
                                                   static_cast<s64>(entries[i].offset), 0, "anon");
            break;
        }
        case MemoryOpTypes::ORBIS_KERNEL_MAP_OP_UNMAP: {
            result = sceKernelMunmap(entries[i].start, entries[i].length);
            break;
        }
        case MemoryOpTypes::ORBIS_KERNEL_MAP_OP_PROTECT: {
            result = sceKernelMprotect(entries[i].start, entries[i].length, entries[i].protection);
            break;
        }
        case MemoryOpTypes::ORBIS_KERNEL_MAP_OP_MAP_FLEXIBLE: {
            result = sceKernelMapNamedFlexibleMemory(&entries[i].start, entries[i].length,
                                                     entries[i].protection, flags, "anon");
            break;
        }
        case MemoryOpTypes::ORBIS_KERNEL_MAP_OP_TYPE_PROTECT: {
            result = sceKernelMtypeprotect(entries[i].start, entries[i].length, entries[i].type,
                                           entries[i].protection);
            break;
        }
        default: {
            UNREACHABLE();
        }
        }

        if (result != ORBIS_OK) {
            LOG_ERROR(Kernel_Vmm, "failed with error code {:#x}", result);
            break;
        }
    }
    if (numEntriesOut != NULL) {
        *numEntriesOut = processed;
    }
    return result;
}

s32 PS4_SYSV_ABI sceKernelSetVirtualRangeName(const void* addr, u64 len, const char* name) {
    if (name == nullptr) {
        LOG_ERROR(Kernel_Vmm, "name is invalid!");
        return ORBIS_KERNEL_ERROR_EFAULT;
    }

    if (std::strlen(name) >= ORBIS_KERNEL_MAXIMUM_NAME_LENGTH) {
        LOG_ERROR(Kernel_Vmm, "name exceeds 32 bytes!");
        return ORBIS_KERNEL_ERROR_ENAMETOOLONG;
    }

    auto* memory = Core::Memory::Instance();
    memory->NameVirtualRange(std::bit_cast<VAddr>(addr), len, name);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceKernelMemoryPoolExpand(u64 searchStart, u64 searchEnd, u64 len, u64 alignment,
                                           u64* physAddrOut) {
    if (searchStart < 0 || searchEnd <= searchStart) {
        LOG_ERROR(Kernel_Vmm, "Provided address range is invalid!");
        return ORBIS_KERNEL_ERROR_EINVAL;
    }
    if (len <= 0 || !Common::Is64KBAligned(len)) {
        LOG_ERROR(Kernel_Vmm, "Provided length {:#x} is invalid!", len);
        return ORBIS_KERNEL_ERROR_EINVAL;
    }
    if (alignment != 0 && !Common::Is64KBAligned(alignment)) {
        LOG_ERROR(Kernel_Vmm, "Alignment {:#x} is invalid!", alignment);
        return ORBIS_KERNEL_ERROR_EINVAL;
    }
    if (physAddrOut == nullptr) {
        LOG_ERROR(Kernel_Vmm, "Result physical address pointer is null!");
        return ORBIS_KERNEL_ERROR_EINVAL;
    }

    const bool is_in_range = searchEnd - searchStart >= len;
    if (searchEnd <= searchStart || searchEnd < len || !is_in_range) {
        LOG_ERROR(Kernel_Vmm,
                  "Provided address range is too small!"
                  " searchStart = {:#x}, searchEnd = {:#x}, length = {:#x}",
                  searchStart, searchEnd, len);
        return ORBIS_KERNEL_ERROR_ENOMEM;
    }

    auto* memory = Core::Memory::Instance();
    PAddr phys_addr = memory->PoolExpand(searchStart, searchEnd, len, alignment);
    if (phys_addr == -1) {
        return ORBIS_KERNEL_ERROR_ENOMEM;
    }

    *physAddrOut = static_cast<s64>(phys_addr);

    LOG_INFO(Kernel_Vmm,
             "searchStart = {:#x}, searchEnd = {:#x}, len = {:#x}, alignment = {:#x}, physAddrOut "
             "= {:#x}",
             searchStart, searchEnd, len, alignment, phys_addr);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceKernelMemoryPoolReserve(void* addr_in, u64 len, u64 alignment, s32 flags,
                                            void** addr_out) {
    LOG_INFO(Kernel_Vmm, "addr_in = {}, len = {:#x}, alignment = {:#x}, flags = {:#x}",
             fmt::ptr(addr_in), len, alignment, flags);

    if (len == 0 || !Common::Is2MBAligned(len)) {
        LOG_ERROR(Kernel_Vmm, "Map size is either zero or not 2MB aligned!");
        return ORBIS_KERNEL_ERROR_EINVAL;
    }
    if (alignment != 0) {
        if ((!std::has_single_bit(alignment) && !Common::Is2MBAligned(alignment))) {
            LOG_ERROR(Kernel_Vmm, "Alignment value is invalid!");
            return ORBIS_KERNEL_ERROR_EINVAL;
        }
    }

    auto* memory = Core::Memory::Instance();
    const VAddr in_addr = reinterpret_cast<VAddr>(addr_in);
    const auto map_flags = static_cast<Core::MemoryMapFlags>(flags);
    u64 map_alignment = alignment == 0 ? 2_MB : alignment;

    return memory->MapMemory(addr_out, std::bit_cast<VAddr>(addr_in), len,
                             Core::MemoryProt::NoAccess, map_flags, Core::VMAType::PoolReserved,
                             "anon", false, -1, map_alignment);
}

s32 PS4_SYSV_ABI sceKernelMemoryPoolCommit(void* addr, u64 len, s32 type, s32 prot, s32 flags) {
    if (addr == nullptr) {
        LOG_ERROR(Kernel_Vmm, "Address is invalid!");
        return ORBIS_KERNEL_ERROR_EINVAL;
    }
    if (len == 0 || !Common::Is64KBAligned(len)) {
        LOG_ERROR(Kernel_Vmm, "Map size is either zero or not 64KB aligned!");
        return ORBIS_KERNEL_ERROR_EINVAL;
    }

    const auto mem_prot = static_cast<Core::MemoryProt>(prot);
    if (True(mem_prot & Core::MemoryProt::CpuExec)) {
        LOG_ERROR(Kernel_Vmm, "Executable permissions are not allowed.");
        return ORBIS_KERNEL_ERROR_EINVAL;
    }

    LOG_INFO(Kernel_Vmm, "addr = {}, len = {:#x}, type = {:#x}, prot = {:#x}, flags = {:#x}",
             fmt::ptr(addr), len, type, prot, flags);

    const VAddr in_addr = reinterpret_cast<VAddr>(addr);
    auto* memory = Core::Memory::Instance();
    return memory->PoolCommit(in_addr, len, mem_prot, type);
}

s32 PS4_SYSV_ABI sceKernelMemoryPoolDecommit(void* addr, u64 len, s32 flags) {
    if (addr == nullptr) {
        LOG_ERROR(Kernel_Vmm, "Address is invalid!");
        return ORBIS_KERNEL_ERROR_EINVAL;
    }
    if (len == 0 || !Common::Is64KBAligned(len)) {
        LOG_ERROR(Kernel_Vmm, "Map size is either zero or not 64KB aligned!");
        return ORBIS_KERNEL_ERROR_EINVAL;
    }

    LOG_INFO(Kernel_Vmm, "addr = {}, len = {:#x}, flags = {:#x}", fmt::ptr(addr), len, flags);

    const VAddr pool_addr = reinterpret_cast<VAddr>(addr);
    auto* memory = Core::Memory::Instance();

    return memory->PoolDecommit(pool_addr, len);
}

s32 PS4_SYSV_ABI sceKernelMemoryPoolBatch(const OrbisKernelMemoryPoolBatchEntry* entries, s32 count,
                                          s32* num_processed, s32 flags) {
    if (entries == nullptr) {
        return ORBIS_KERNEL_ERROR_EINVAL;
    }
    s32 result = ORBIS_OK;
    s32 processed = 0;

    for (s32 i = 0; i < count; i++, processed++) {
        OrbisKernelMemoryPoolBatchEntry entry = entries[i];
        switch (entry.opcode) {
        case OrbisKernelMemoryPoolOpcode::Commit: {
            result = sceKernelMemoryPoolCommit(entry.commit_params.addr, entry.commit_params.len,
                                               entry.commit_params.type, entry.commit_params.prot,
                                               entry.flags);
            break;
        }
        case OrbisKernelMemoryPoolOpcode::Decommit: {
            result = sceKernelMemoryPoolDecommit(entry.decommit_params.addr,
                                                 entry.decommit_params.len, entry.flags);
            break;
        }
        case OrbisKernelMemoryPoolOpcode::Protect: {
            result = sceKernelMprotect(entry.protect_params.addr, entry.protect_params.len,
                                       entry.protect_params.prot);
            break;
        }
        case OrbisKernelMemoryPoolOpcode::TypeProtect: {
            result = sceKernelMtypeprotect(
                entry.type_protect_params.addr, entry.type_protect_params.len,
                entry.type_protect_params.type, entry.type_protect_params.prot);
            break;
        }
        case OrbisKernelMemoryPoolOpcode::Move: {
            UNREACHABLE_MSG("Unimplemented sceKernelMemoryPoolBatch opcode Move");
        }
        default: {
            result = ORBIS_KERNEL_ERROR_EINVAL;
            break;
        }
        }

        if (result != ORBIS_OK) {
            break;
        }
    }

    if (num_processed != nullptr) {
        *num_processed = processed;
    }
    return result;
}

s32 PS4_SYSV_ABI sceKernelMemoryPoolGetBlockStats(OrbisKernelMemoryPoolBlockStats* stats,
                                                  u64 size) {
    LOG_WARNING(Kernel_Vmm, "called");
    auto* memory = Core::Memory::Instance();
    OrbisKernelMemoryPoolBlockStats local_stats;
    memory->GetMemoryPoolStats(&local_stats);

    u64 size_to_copy = size < sizeof(OrbisKernelMemoryPoolBlockStats)
                           ? size
                           : sizeof(OrbisKernelMemoryPoolBlockStats);
    ASSERT_MSG(stats != nullptr || size == 0, "Block stats cannot be null");
    std::memcpy(stats, &local_stats, size_to_copy);
    return ORBIS_OK;
}

#ifdef __ANDROID__
extern "C" int executor_lsx4_android_fault_signal_owner() __attribute__((weak));
extern "C" int executor_lsx4_android_fex_signal_api_active() __attribute__((weak));
extern "C" int executor_lsx4_android_fex_publish_dynamic_executable_range(
    std::uint64_t address, std::uint64_t size, std::uint64_t range_cookie,
    std::uint32_t protection, std::uint32_t range_flags) __attribute__((weak));
extern "C" int executor_lsx4_android_fex_invalidate_dynamic_executable_range(
    std::uint64_t address, std::uint64_t size, std::uint64_t range_cookie,
    std::uint32_t invalidate_flags) __attribute__((weak));

namespace {

constexpr std::uint32_t ExecutorFexRangeExecutable = 1u << 0;
constexpr std::uint32_t ExecutorFexRangeWritable = 1u << 1;
constexpr std::uint32_t ExecutorFexRangeSelfModifying = 1u << 3;
constexpr std::uint32_t ExecutorFexInvalidateTranslations = 1u << 0;
constexpr std::uint32_t ExecutorFexRemoveRegistrations = 1u << 1;
constexpr std::uint64_t ExecutorDynamicRangeCookieDomain =
    UINT64_C(0x44594e4d41505258); // "DYNMAPRX"

struct ExecutorHostMmapRange {
    u64 size{};
    Core::MemoryProt protection{Core::MemoryProt::NoAccess};
    std::uint64_t executable_cookie{};
};

struct ExecutorHostMmapSpan {
    VAddr address{};
    u64 size{};
    Core::MemoryProt protection{Core::MemoryProt::NoAccess};
    std::uint64_t executable_cookie{};
};

static std::mutex g_host_mmap_mutex;
static std::map<VAddr, ExecutorHostMmapRange> g_host_mmap_ranges;
static std::uint64_t g_dynamic_range_cookie_serial = 1;

static bool ExecutorRangeEnd(const VAddr address, const u64 size, VAddr& end) {
    if (size == 0 || size > std::numeric_limits<VAddr>::max() - address) {
        return false;
    }
    end = address + size;
    return true;
}

static bool ExecutorRangeExecutable(const Core::MemoryProt protection) {
    return True(protection & Core::MemoryProt::CpuExec);
}

static bool ExecutorRangeWritable(const Core::MemoryProt protection) {
    return True(protection & Core::MemoryProt::CpuWrite);
}

static std::uint32_t ExecutorFexProtection(const Core::MemoryProt protection) {
    return static_cast<std::uint32_t>(protection) & 0x7u;
}

static std::uint32_t ExecutorFexRangeFlags(const Core::MemoryProt protection) {
    std::uint32_t flags = 0;
    if (ExecutorRangeExecutable(protection)) {
        flags |= ExecutorFexRangeExecutable;
    }
    if (ExecutorRangeWritable(protection)) {
        flags |= ExecutorFexRangeWritable;
    }
    if ((flags & (ExecutorFexRangeExecutable | ExecutorFexRangeWritable)) ==
        (ExecutorFexRangeExecutable | ExecutorFexRangeWritable)) {
        flags |= ExecutorFexRangeSelfModifying;
    }
    return flags;
}

static std::uint64_t ExecutorMixDynamicRangeCookie(std::uint64_t value) {
    value ^= value >> 30;
    value *= UINT64_C(0xbf58476d1ce4e5b9);
    value ^= value >> 27;
    value *= UINT64_C(0x94d049bb133111eb);
    return value ^ (value >> 31);
}

static std::uint64_t ExecutorAllocateDynamicRangeCookieLocked() {
    while (g_dynamic_range_cookie_serial != 0 &&
           g_dynamic_range_cookie_serial != std::numeric_limits<std::uint64_t>::max()) {
        const std::uint64_t serial = g_dynamic_range_cookie_serial++;
        const std::uint64_t cookie =
            ExecutorMixDynamicRangeCookie(ExecutorDynamicRangeCookieDomain ^ serial);
        // The mixer is a permutation over 64-bit values. Skipping its sole
        // zero result therefore preserves uniqueness for every live process.
        if (cookie != 0) {
            return cookie;
        }
    }
    return 0;
}

static bool ExecutorFexDynamicRangeActive() {
    return executor_lsx4_android_fault_signal_owner != nullptr &&
           executor_lsx4_android_fault_signal_owner() == 2 &&
           executor_lsx4_android_fex_signal_api_active != nullptr &&
           executor_lsx4_android_fex_signal_api_active() != 0;
}

static int ExecutorPublishFexDynamicRange(const ExecutorHostMmapSpan& span,
                                          const char* source) {
    if (!ExecutorRangeExecutable(span.protection) || span.size == 0) {
        return 0;
    }
    if (!ExecutorFexDynamicRangeActive()) {
        return 0;
    }
    if (span.executable_cookie == 0 ||
        executor_lsx4_android_fex_publish_dynamic_executable_range == nullptr) {
        __android_log_print(
            ANDROID_LOG_ERROR, "LSX4Native",
            "[EXECUTOR_FEX_DYNAMIC_EXEC_RANGE] op=publish source=%s addr=0x%llx "
            "size=0x%llx cookie=0x%llx result=missing_bridge",
            source, static_cast<unsigned long long>(span.address),
            static_cast<unsigned long long>(span.size),
            static_cast<unsigned long long>(span.executable_cookie));
        return -4;
    }
    const std::uint32_t protection = ExecutorFexProtection(span.protection);
    const std::uint32_t range_flags = ExecutorFexRangeFlags(span.protection);
    const int rc = executor_lsx4_android_fex_publish_dynamic_executable_range(
        span.address, span.size, span.executable_cookie, protection, range_flags);
    __android_log_print(
        rc == 0 ? ANDROID_LOG_INFO : ANDROID_LOG_ERROR, "LSX4Native",
        "[EXECUTOR_FEX_DYNAMIC_EXEC_RANGE] op=publish source=%s addr=0x%llx "
        "size=0x%llx cookie=0x%llx prot=0x%x flags=0x%x rc=%d",
        source, static_cast<unsigned long long>(span.address),
        static_cast<unsigned long long>(span.size),
        static_cast<unsigned long long>(span.executable_cookie), protection,
        range_flags, rc);
    return rc;
}

static int ExecutorInvalidateFexDynamicRange(const ExecutorHostMmapSpan& span,
                                             const std::uint32_t flags,
                                             const char* source) {
    if (!ExecutorRangeExecutable(span.protection) || span.size == 0) {
        return 0;
    }
    if (!ExecutorFexDynamicRangeActive()) {
        return 0;
    }
    if (span.executable_cookie == 0 ||
        executor_lsx4_android_fex_invalidate_dynamic_executable_range == nullptr) {
        __android_log_print(
            ANDROID_LOG_ERROR, "LSX4Native",
            "[EXECUTOR_FEX_DYNAMIC_EXEC_RANGE] op=invalidate source=%s addr=0x%llx "
            "size=0x%llx cookie=0x%llx flags=0x%x result=missing_bridge",
            source, static_cast<unsigned long long>(span.address),
            static_cast<unsigned long long>(span.size),
            static_cast<unsigned long long>(span.executable_cookie), flags);
        return -4;
    }
    const int rc = executor_lsx4_android_fex_invalidate_dynamic_executable_range(
        span.address, span.size, span.executable_cookie, flags);
    __android_log_print(
        rc == 0 ? ANDROID_LOG_INFO : ANDROID_LOG_ERROR, "LSX4Native",
        "[EXECUTOR_FEX_DYNAMIC_EXEC_RANGE] op=invalidate source=%s addr=0x%llx "
        "size=0x%llx cookie=0x%llx flags=0x%x rc=%d",
        source, static_cast<unsigned long long>(span.address),
        static_cast<unsigned long long>(span.size),
        static_cast<unsigned long long>(span.executable_cookie), flags, rc);
    return rc;
}

static std::vector<ExecutorHostMmapSpan> ExecutorOverlappingHostRangesLocked(
    const VAddr begin, const VAddr end) {
    std::vector<ExecutorHostMmapSpan> spans;
    auto it = g_host_mmap_ranges.upper_bound(begin);
    if (it != g_host_mmap_ranges.begin()) {
        --it;
    }
    for (; it != g_host_mmap_ranges.end(); ++it) {
        VAddr span_end = 0;
        if (!ExecutorRangeEnd(it->first, it->second.size, span_end)) {
            continue;
        }
        if (span_end <= begin) {
            continue;
        }
        if (it->first >= end) {
            break;
        }
        spans.push_back({it->first, it->second.size, it->second.protection,
                         it->second.executable_cookie});
    }
    return spans;
}

static bool ExecutorRangesCover(const std::vector<ExecutorHostMmapSpan>& spans,
                                const VAddr begin, const VAddr end) {
    VAddr cursor = begin;
    for (const auto& span : spans) {
        VAddr span_end = 0;
        if (!ExecutorRangeEnd(span.address, span.size, span_end) || span_end <= cursor) {
            continue;
        }
        if (span.address > cursor) {
            return false;
        }
        cursor = std::min(end, span_end);
        if (cursor == end) {
            return true;
        }
    }
    return cursor == end;
}

static bool ExecutorAssignExecutableCookiesLocked(
    std::vector<ExecutorHostMmapSpan>& spans) {
    for (auto& span : spans) {
        if (!ExecutorRangeExecutable(span.protection)) {
            span.executable_cookie = 0;
            continue;
        }
        span.executable_cookie = ExecutorAllocateDynamicRangeCookieLocked();
        if (span.executable_cookie == 0) {
            return false;
        }
    }
    return true;
}

static std::vector<ExecutorHostMmapSpan> ExecutorReplaceHostRangeLocked(
    const std::vector<ExecutorHostMmapSpan>& old_spans, const VAddr begin,
    const VAddr end, const Core::MemoryProt replacement,
    const bool include_replacement) {
    std::vector<ExecutorHostMmapSpan> next;
    next.reserve(old_spans.size() * 3 + (old_spans.empty() && include_replacement ? 1 : 0));
    VAddr replacement_cursor = begin;
    for (const auto& old : old_spans) {
        const VAddr old_end = old.address + old.size;
        if (old.address < begin) {
            next.push_back({old.address, begin - old.address, old.protection, 0});
        }
        const VAddr intersection_begin = std::max(old.address, begin);
        const VAddr intersection_end = std::min(old_end, end);
        if (include_replacement && intersection_begin < intersection_end) {
            if (replacement_cursor < intersection_begin) {
                next.push_back({replacement_cursor, intersection_begin - replacement_cursor,
                                replacement, 0});
            }
            next.push_back({intersection_begin, intersection_end - intersection_begin,
                            replacement, 0});
            replacement_cursor = intersection_end;
        }
        if (old_end > end) {
            next.push_back({end, old_end - end, old.protection, 0});
        }
    }
    if (include_replacement && replacement_cursor < end) {
        next.push_back({replacement_cursor, end - replacement_cursor, replacement, 0});
    }
    return next;
}

static void ExecutorEraseHostRangesLocked(
    const std::vector<ExecutorHostMmapSpan>& spans) {
    for (const auto& span : spans) {
        g_host_mmap_ranges.erase(span.address);
    }
}

static void ExecutorInsertHostRangesLocked(
    const std::vector<ExecutorHostMmapSpan>& spans) {
    for (const auto& span : spans) {
        g_host_mmap_ranges.emplace(
            span.address,
            ExecutorHostMmapRange{span.size, span.protection, span.executable_cookie});
    }
}

static bool ExecutorRemoveExecutableRanges(
    const std::vector<ExecutorHostMmapSpan>& spans, const char* source) {
    bool ok = true;
    for (const auto& span : spans) {
        if (ExecutorRangeExecutable(span.protection) &&
            ExecutorInvalidateFexDynamicRange(
                span, ExecutorFexInvalidateTranslations |
                          ExecutorFexRemoveRegistrations,
                source) != 0) {
            ok = false;
        }
    }
    return ok;
}

static bool ExecutorPublishExecutableRanges(
    const std::vector<ExecutorHostMmapSpan>& spans, std::size_t& published_count,
    const char* source) {
    published_count = 0;
    for (const auto& span : spans) {
        if (!ExecutorRangeExecutable(span.protection)) {
            continue;
        }
        if (ExecutorPublishFexDynamicRange(span, source) != 0) {
            return false;
        }
        ++published_count;
    }
    return true;
}

static void ExecutorRemovePublishedExecutableRanges(
    const std::vector<ExecutorHostMmapSpan>& spans, std::size_t published_count,
    const char* source) {
    for (const auto& span : spans) {
        if (!ExecutorRangeExecutable(span.protection) || published_count == 0) {
            continue;
        }
        (void)ExecutorInvalidateFexDynamicRange(
            span, ExecutorFexInvalidateTranslations |
                      ExecutorFexRemoveRegistrations,
            source);
        --published_count;
    }
}

static bool ExecutorRestoreHostProtections(
    const std::vector<ExecutorHostMmapSpan>& old_spans, const VAddr begin,
    const VAddr end) {
    bool ok = true;
    for (const auto& old : old_spans) {
        const VAddr old_end = old.address + old.size;
        const VAddr restore_begin = std::max(old.address, begin);
        const VAddr restore_end = std::min(old_end, end);
        if (restore_begin < restore_end &&
            ::mprotect(reinterpret_cast<void*>(restore_begin),
                       restore_end - restore_begin,
                       ExecutorToHostProt(old.protection)) != 0) {
            ok = false;
        }
    }
    return ok;
}

} // namespace

extern "C" __attribute__((visibility("default"), used)) int
executor_lsx4_android_is_live_dynamic_guest_executable_address(
    const std::uint64_t address) {
    if (address == 0) {
        return 0;
    }

    std::scoped_lock lock{g_host_mmap_mutex};
    auto it = g_host_mmap_ranges.upper_bound(address);
    if (it == g_host_mmap_ranges.begin()) {
        return 0;
    }
    --it;

    VAddr end = 0;
    const auto& range = it->second;
    if (!ExecutorRangeEnd(it->first, range.size, end) || address < it->first ||
        address >= end) {
        return 0;
    }

    // The interval map is committed only after mmap/mprotect publication and
    // is erased only after a successful retirement/unmap transaction.  A
    // non-zero executable cookie therefore identifies the currently live
    // generation, while RW-only and retired intervals fail closed.
    return ExecutorRangeExecutable(range.protection) &&
                   range.executable_cookie != 0
               ? 1
               : 0;
}

static bool ExecutorHostMmapContains(const VAddr addr) {
    std::scoped_lock lk{g_host_mmap_mutex};
    auto it = g_host_mmap_ranges.upper_bound(addr);
    if (it == g_host_mmap_ranges.begin()) {
        return false;
    }
    --it;
    VAddr end = 0;
    if (!ExecutorRangeEnd(it->first, it->second.size, end)) {
        return false;
    }
    return addr >= it->first && addr < end;
}

static int ExecutorToHostProt(Core::MemoryProt prot) {
    int host_prot = PROT_NONE;
    if (True(prot & Core::MemoryProt::CpuRead)) {
        host_prot |= PROT_READ;
    }
    if (True(prot & Core::MemoryProt::CpuWrite)) {
        host_prot |= PROT_READ | PROT_WRITE;
    }
    if (True(prot & Core::MemoryProt::CpuExec)) {
        host_prot |= PROT_EXEC;
    }
    return host_prot;
}

static s32 ExecutorHostMprotect(const VAddr addr, const u64 size,
                                const Core::MemoryProt protection) {
    VAddr end = 0;
    if (!ExecutorRangeEnd(addr, size, end)) {
        *__Error() = EINVAL;
        return ORBIS_KERNEL_ERROR_EINVAL;
    }

    std::scoped_lock lock{g_host_mmap_mutex};
    const auto old_spans = ExecutorOverlappingHostRangesLocked(addr, end);
    if (!ExecutorRangesCover(old_spans, addr, end)) {
        *__Error() = EINVAL;
        return ORBIS_KERNEL_ERROR_EINVAL;
    }

    const bool protection_changes = std::any_of(
        old_spans.begin(), old_spans.end(),
        [protection](const auto& span) { return span.protection != protection; });
    if (!protection_changes) {
        if (::mprotect(reinterpret_cast<void*>(addr), size,
                       ExecutorToHostProt(protection)) != 0) {
            *__Error() = errno;
            return ORBIS_KERNEL_ERROR_EINVAL;
        }
        if (ExecutorRangeExecutable(protection) &&
            ExecutorRangeWritable(protection)) {
            for (const auto& old : old_spans) {
                const VAddr old_end = old.address + old.size;
                ExecutorHostMmapSpan changed{
                    std::max(old.address, addr),
                    std::min(old_end, end) - std::max(old.address, addr),
                    old.protection, old.executable_cookie};
                if (changed.size != 0 &&
                    ExecutorInvalidateFexDynamicRange(
                        changed, ExecutorFexInvalidateTranslations,
                        "mprotect_wx_barrier") != 0) {
                    *__Error() = EIO;
                    return ORBIS_KERNEL_ERROR_EIO;
                }
            }
        }
        return ORBIS_OK;
    }

    auto next_spans =
        ExecutorReplaceHostRangeLocked(old_spans, addr, end, protection, true);
    if (!ExecutorAssignExecutableCookiesLocked(next_spans)) {
        *__Error() = EOVERFLOW;
        return ORBIS_KERNEL_ERROR_EOVERFLOW;
    }
    if (!ExecutorRemoveExecutableRanges(old_spans, "mprotect_remove_old")) {
        std::size_t restored = 0;
        (void)ExecutorPublishExecutableRanges(old_spans, restored,
                                              "mprotect_remove_rollback");
        *__Error() = EIO;
        return ORBIS_KERNEL_ERROR_EIO;
    }
    if (::mprotect(reinterpret_cast<void*>(addr), size,
                   ExecutorToHostProt(protection)) != 0) {
        const int saved_errno = errno;
        std::size_t restored = 0;
        (void)ExecutorPublishExecutableRanges(old_spans, restored,
                                              "mprotect_os_rollback");
        *__Error() = saved_errno;
        return ORBIS_KERNEL_ERROR_EINVAL;
    }

    std::size_t published = 0;
    if (!ExecutorPublishExecutableRanges(next_spans, published,
                                         "mprotect_publish_new")) {
        ExecutorRemovePublishedExecutableRanges(next_spans, published,
                                                "mprotect_publish_cleanup");
        const bool host_restored =
            ExecutorRestoreHostProtections(old_spans, addr, end);
        if (host_restored) {
            std::size_t restored = 0;
            (void)ExecutorPublishExecutableRanges(old_spans, restored,
                                                  "mprotect_publish_rollback");
        }
        *__Error() = EIO;
        return ORBIS_KERNEL_ERROR_EIO;
    }

    ExecutorEraseHostRangesLocked(old_spans);
    ExecutorInsertHostRangesLocked(next_spans);
    __android_log_print(
        ANDROID_LOG_INFO, "LSX4Native",
        "[EXECUTOR_HOST_MPROTECT] addr=0x%llx size=0x%llx prot=0x%x "
        "oldIntervals=%zu newIntervals=%zu",
        static_cast<unsigned long long>(addr),
        static_cast<unsigned long long>(size),
        static_cast<unsigned int>(protection), old_spans.size(),
        next_spans.size());
    return ORBIS_OK;
}

static void* ExecutorHostMmap(const VAddr requested_addr, const u64 size,
                              const Core::MemoryProt protection,
                              const bool fixed) {
    VAddr requested_end = 0;
    if (size == 0 || (fixed && !ExecutorRangeEnd(requested_addr, size, requested_end))) {
        *__Error() = EINVAL;
        return MAP_FAILED;
    }

    std::scoped_lock lock{g_host_mmap_mutex};
    std::vector<ExecutorHostMmapSpan> old_spans;
    std::vector<ExecutorHostMmapSpan> survivors;
    if (fixed) {
        old_spans = ExecutorOverlappingHostRangesLocked(requested_addr, requested_end);
        survivors = ExecutorReplaceHostRangeLocked(
            old_spans, requested_addr, requested_end, Core::MemoryProt::NoAccess,
            false);
        if (!ExecutorAssignExecutableCookiesLocked(survivors)) {
            *__Error() = EOVERFLOW;
            return MAP_FAILED;
        }
        if (!ExecutorRemoveExecutableRanges(old_spans, "mmap_fixed_remove_old")) {
            std::size_t restored = 0;
            (void)ExecutorPublishExecutableRanges(old_spans, restored,
                                                  "mmap_fixed_remove_rollback");
            *__Error() = EIO;
            return MAP_FAILED;
        }
    }

    const int host_flags = MAP_PRIVATE | MAP_ANONYMOUS | (fixed ? MAP_FIXED : 0);
    void* const host_addr =
        ::mmap(fixed ? reinterpret_cast<void*>(requested_addr) : nullptr, size,
               ExecutorToHostProt(protection), host_flags, -1, 0);
    if (host_addr == MAP_FAILED) {
        const int saved_errno = errno;
        if (fixed) {
            std::size_t restored = 0;
            (void)ExecutorPublishExecutableRanges(old_spans, restored,
                                                  "mmap_fixed_os_rollback");
        }
        *__Error() = saved_errno;
        return MAP_FAILED;
    }

    const VAddr mapped_addr = reinterpret_cast<VAddr>(host_addr);
    VAddr mapped_end = 0;
    if (!ExecutorRangeEnd(mapped_addr, size, mapped_end) ||
        (fixed && mapped_addr != requested_addr) ||
        (!fixed && !ExecutorOverlappingHostRangesLocked(mapped_addr, mapped_end).empty())) {
        (void)::munmap(host_addr, size);
        if (fixed) {
            std::size_t survivor_published = 0;
            (void)ExecutorPublishExecutableRanges(
                survivors, survivor_published, "mmap_geometry_survivors");
            ExecutorEraseHostRangesLocked(old_spans);
            ExecutorInsertHostRangesLocked(survivors);
        }
        *__Error() = EIO;
        return MAP_FAILED;
    }

    std::vector<ExecutorHostMmapSpan> next_spans = survivors;
    next_spans.push_back({mapped_addr, size, protection, 0});
    if (!ExecutorAssignExecutableCookiesLocked(next_spans)) {
        (void)::munmap(host_addr, size);
        if (fixed) {
            std::size_t survivor_published = 0;
            (void)ExecutorPublishExecutableRanges(
                survivors, survivor_published, "mmap_cookie_survivors");
            ExecutorEraseHostRangesLocked(old_spans);
            ExecutorInsertHostRangesLocked(survivors);
        }
        *__Error() = EOVERFLOW;
        return MAP_FAILED;
    }

    std::size_t published = 0;
    if (!ExecutorPublishExecutableRanges(next_spans, published,
                                         "mmap_publish_new")) {
        ExecutorRemovePublishedExecutableRanges(next_spans, published,
                                                "mmap_publish_cleanup");
        (void)::munmap(host_addr, size);
        if (fixed) {
            std::size_t survivor_published = 0;
            (void)ExecutorPublishExecutableRanges(survivors, survivor_published,
                                                  "mmap_publish_survivors");
            ExecutorEraseHostRangesLocked(old_spans);
            ExecutorInsertHostRangesLocked(survivors);
        }
        *__Error() = EIO;
        return MAP_FAILED;
    }

    if (fixed) {
        ExecutorEraseHostRangesLocked(old_spans);
    }
    ExecutorInsertHostRangesLocked(next_spans);
    return host_addr;
}

static s32 ExecutorHostMunmap(const VAddr addr, const u64 size) {
    VAddr end = 0;
    if (!ExecutorRangeEnd(addr, size, end)) {
        *__Error() = EINVAL;
        return ORBIS_KERNEL_ERROR_EINVAL;
    }

    std::scoped_lock lock{g_host_mmap_mutex};
    const auto old_spans = ExecutorOverlappingHostRangesLocked(addr, end);
    if (!ExecutorRangesCover(old_spans, addr, end)) {
        *__Error() = EINVAL;
        return ORBIS_KERNEL_ERROR_EINVAL;
    }
    auto survivors = ExecutorReplaceHostRangeLocked(
        old_spans, addr, end, Core::MemoryProt::NoAccess, false);
    if (!ExecutorAssignExecutableCookiesLocked(survivors)) {
        *__Error() = EOVERFLOW;
        return ORBIS_KERNEL_ERROR_EOVERFLOW;
    }
    if (!ExecutorRemoveExecutableRanges(old_spans, "munmap_remove_old")) {
        std::size_t restored = 0;
        (void)ExecutorPublishExecutableRanges(old_spans, restored,
                                              "munmap_remove_rollback");
        *__Error() = EIO;
        return ORBIS_KERNEL_ERROR_EIO;
    }

    std::size_t survivors_published = 0;
    if (!ExecutorPublishExecutableRanges(survivors, survivors_published,
                                         "munmap_publish_survivors")) {
        ExecutorRemovePublishedExecutableRanges(
            survivors, survivors_published, "munmap_publish_cleanup");
        std::size_t restored = 0;
        (void)ExecutorPublishExecutableRanges(old_spans, restored,
                                              "munmap_publish_rollback");
        *__Error() = EIO;
        return ORBIS_KERNEL_ERROR_EIO;
    }

    if (::munmap(reinterpret_cast<void*>(addr), size) != 0) {
        const int saved_errno = errno;
        ExecutorRemovePublishedExecutableRanges(
            survivors, survivors_published, "munmap_os_cleanup");
        std::size_t restored = 0;
        (void)ExecutorPublishExecutableRanges(old_spans, restored,
                                              "munmap_os_rollback");
        *__Error() = saved_errno;
        return ORBIS_KERNEL_ERROR_EINVAL;
    }

    ExecutorEraseHostRangesLocked(old_spans);
    ExecutorInsertHostRangesLocked(survivors);
    return ORBIS_OK;
}
#endif

void* PS4_SYSV_ABI posix_mmap(void* addr, u64 len, s32 prot, s32 flags, s32 fd, s64 phys_addr) {
    LOG_INFO(
        Kernel_Vmm,
        "called addr = {}, len = {:#x}, prot = {:#x}, flags = {:#x}, fd = {}, phys_addr = {:#x}",
        fmt::ptr(addr), len, prot, flags, fd, phys_addr);

    if (len == 0) {
        ErrSceToPosix(ORBIS_KERNEL_ERROR_EINVAL);
#ifdef __ANDROID__
        ExecutorLiveMmapLog("posix_mmap_len0", addr, len, prot, flags, fd, phys_addr,
                            reinterpret_cast<void*>(-1), *__Error(),
                            ORBIS_KERNEL_ERROR_EINVAL);
#endif
        return reinterpret_cast<void*>(-1);
    }

    void* addr_out;
    auto* memory = Core::Memory::Instance();
    const auto mem_prot = static_cast<Core::MemoryProt>(prot);
    const auto mem_flags = static_cast<Core::MemoryMapFlags>(flags);
    const auto vaddr = reinterpret_cast<VAddr>(addr);

    const VAddr aligned_addr = Common::AlignUp(vaddr, 16_KB);
    const u64 aligned_size = Common::AlignUp(len, 16_KB);

#ifdef __ANDROID__
    if ((True(mem_flags & Core::MemoryMapFlags::Anon) ||
         True(mem_flags & Core::MemoryMapFlags::Stack)) &&
        fd == -1) {
        const bool fixed = True(mem_flags & Core::MemoryMapFlags::Fixed);
        void* host_addr = ExecutorHostMmap(aligned_addr, aligned_size, mem_prot, fixed);
        if (host_addr == MAP_FAILED) {
#ifdef __ANDROID__
            ExecutorLiveMmapLog("posix_mmap_host_fail", addr, len, prot, flags, fd, phys_addr,
                                reinterpret_cast<void*>(-1), *__Error(), -1);
#endif
            return reinterpret_cast<void*>(-1);
        }
#ifdef __ANDROID__
        ExecutorLiveMmapLog("posix_mmap_host_ok", addr, len, prot, flags, fd, phys_addr,
                            host_addr, 0, ORBIS_OK);
#endif
        return host_addr;
    }
#endif

    if (True(mem_flags & Core::MemoryMapFlags::Fixed) && vaddr != aligned_addr) {
        ErrSceToPosix(ORBIS_KERNEL_ERROR_EINVAL);
#ifdef __ANDROID__
        ExecutorLiveMmapLog("posix_mmap_fixed_unaligned", addr, len, prot, flags, fd, phys_addr,
                            reinterpret_cast<void*>(-1), *__Error(),
                            ORBIS_KERNEL_ERROR_EINVAL);
#endif
        return reinterpret_cast<void*>(-1);
    }

    s32 result = ORBIS_OK;
    if (True(mem_flags & Core::MemoryMapFlags::Anon)) {
        result = memory->MapMemory(&addr_out, aligned_addr, aligned_size, mem_prot, mem_flags,
                                   Core::VMAType::Flexible, "anon", false);
    } else if (True(mem_flags & Core::MemoryMapFlags::Stack)) {
        result = memory->MapMemory(&addr_out, aligned_addr, aligned_size, mem_prot, mem_flags,
                                   Core::VMAType::Stack, "anon", false);
    } else if (True(mem_flags & Core::MemoryMapFlags::Void)) {
        result =
            memory->MapMemory(&addr_out, aligned_addr, aligned_size, Core::MemoryProt::NoAccess,
                              mem_flags, Core::VMAType::Reserved, "anon", false);
    } else {
        result = memory->MapFile(&addr_out, aligned_addr, aligned_size, mem_prot, mem_flags, fd,
                                 phys_addr);
    }

    if (result != ORBIS_OK) {
        ErrSceToPosix(result);
#ifdef __ANDROID__
        ExecutorLiveMmapLog("posix_mmap_vm_fail", addr, len, prot, flags, fd, phys_addr,
                            reinterpret_cast<void*>(-1), *__Error(), result);
#endif
        return reinterpret_cast<void*>(-1);
    }
#ifdef __ANDROID__
    ExecutorLiveMmapLog("posix_mmap_vm_ok", addr, len, prot, flags, fd, phys_addr, addr_out, 0,
                        result);
#endif
    return addr_out;
}

s32 PS4_SYSV_ABI sceKernelMmap(void* addr, u64 len, s32 prot, s32 flags, s32 fd, s64 phys_addr,
                               void** res) {
    void* addr_out = posix_mmap(addr, len, prot, flags, fd, phys_addr);

    if (addr_out == reinterpret_cast<void*>(-1)) {
        LOG_ERROR(Kernel_Fs, "error = {}", *__Error());
        const auto result = ErrnoToSceKernelError(*__Error());
#ifdef __ANDROID__
        ExecutorLiveMmapLog("sceKernelMmap_fail", addr, len, prot, flags, fd, phys_addr, addr_out,
                            *__Error(), result);
#endif
        return result;
    }

    *res = addr_out;
#ifdef __ANDROID__
    ExecutorLiveMmapLog("sceKernelMmap_ok", addr, len, prot, flags, fd, phys_addr, addr_out, 0,
                        ORBIS_OK);
#endif
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceKernelConfiguredFlexibleMemorySize(u64* sizeOut) {
    if (sizeOut == nullptr) {
        return ORBIS_KERNEL_ERROR_EINVAL;
    }

    auto* memory = Core::Memory::Instance();
    *sizeOut = memory->GetTotalFlexibleSize();
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceKernelMunmap(void* addr, u64 len) {
    LOG_INFO(Kernel_Vmm, "addr = {}, len = {:#x}", fmt::ptr(addr), len);
    if (len == 0) {
        return ORBIS_KERNEL_ERROR_EINVAL;
    }
#ifdef __ANDROID__
    if (ExecutorHostMmapContains(std::bit_cast<VAddr>(addr))) {
        const u64 aligned_size = Common::AlignUp(len, 16_KB);
        const s32 result = ExecutorHostMunmap(std::bit_cast<VAddr>(addr), aligned_size);
        static std::atomic<u64> host_unmap_count{};
        const u64 count = host_unmap_count.fetch_add(1, std::memory_order_relaxed) + 1;
        if (count <= 32 || std::has_single_bit(count)) {
            __android_log_print(
                ANDROID_LOG_INFO, "LSX4Native",
                "[EXECUTOR_MUNMAP] host=1 count=%llu addr=%p len=0x%llx result=0x%x",
                static_cast<unsigned long long>(count), addr,
                static_cast<unsigned long long>(aligned_size), static_cast<unsigned>(result));
        }
        return result;
    }
#endif
    auto* memory = Core::Memory::Instance();
    const s32 result = memory->UnmapMemory(std::bit_cast<VAddr>(addr), len);
#ifdef __ANDROID__
    static std::atomic<u64> guest_unmap_count{};
    const u64 count = guest_unmap_count.fetch_add(1, std::memory_order_relaxed) + 1;
    if (count <= 32 || std::has_single_bit(count)) {
        __android_log_print(
            ANDROID_LOG_INFO, "LSX4Native",
            "[EXECUTOR_MUNMAP] host=0 count=%llu addr=%p len=0x%llx result=0x%x",
            static_cast<unsigned long long>(count), addr, static_cast<unsigned long long>(len),
            static_cast<unsigned>(result));
    }
#endif
    return result;
}

s32 PS4_SYSV_ABI posix_munmap(void* addr, u64 len) {
    s32 result = sceKernelMunmap(addr, len);
    if (result < 0) {
        LOG_ERROR(Kernel_Pthread, "posix_munmap: error = {}", result);
        ErrSceToPosix(result);
        return -1;
    }
    return result;
}

static constexpr s32 MAX_PRT_APERTURES = 3;
static constexpr VAddr PRT_AREA_START_ADDR = 0x1000000000;
static constexpr u64 PRT_AREA_SIZE = 0xec00000000;
static std::array<std::pair<VAddr, u64>, MAX_PRT_APERTURES> PrtApertures{};

s32 PS4_SYSV_ABI sceKernelSetPrtAperture(s32 id, VAddr address, u64 size) {
    if (id < 0 || id >= MAX_PRT_APERTURES) {
        return ORBIS_KERNEL_ERROR_EINVAL;
    }

    if (address < PRT_AREA_START_ADDR || address + size > PRT_AREA_START_ADDR + PRT_AREA_SIZE) {
        return ORBIS_KERNEL_ERROR_EINVAL;
    }

    if (address % 4096 != 0) {
        return ORBIS_KERNEL_ERROR_EINVAL;
    }

    LOG_WARNING(Kernel_Vmm,
                "PRT aperture id = {}, address = {:#x}, size = {:#x} is set but not used", id,
                address, size);

    auto* memory = Core::Memory::Instance();
    memory->SetPrtArea(id, address, size);

    PrtApertures[id] = {address, size};
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceKernelGetPrtAperture(s32 id, VAddr* address, u64* size) {
    if (id < 0 || id >= MAX_PRT_APERTURES) {
        return ORBIS_KERNEL_ERROR_EINVAL;
    }

    std::tie(*address, *size) = PrtApertures[id];
    return ORBIS_OK;
}

void RegisterMemory(Core::Loader::SymbolsResolver* sym) {
    ASSERT_MSG(sceKernelGetCompiledSdkVersion(&g_sdk_version) == ORBIS_OK,
               "Failed to get compiled SDK verision.");

    LIB_FUNCTION("usHTMoFoBTM", "libkernel_dmem_aliasing2", 1, "libkernel",
                 sceKernelEnableDmemAliasing);
    LIB_FUNCTION("usHTMoFoBTM", "libkernel", 1, "libkernel", sceKernelEnableDmemAliasing);
    LIB_FUNCTION("rTXw65xmLIA", "libkernel", 1, "libkernel", sceKernelAllocateDirectMemory);
    LIB_FUNCTION("B+vc2AO2Zrc", "libkernel", 1, "libkernel", sceKernelAllocateMainDirectMemory);
    LIB_FUNCTION("C0f7TJcbfac", "libkernel", 1, "libkernel", sceKernelAvailableDirectMemorySize);
    LIB_FUNCTION("hwVSPCmp5tM", "libkernel", 1, "libkernel", sceKernelCheckedReleaseDirectMemory);
    LIB_FUNCTION("rVjRvHJ0X6c", "libkernel", 1, "libkernel", sceKernelVirtualQuery);
    LIB_FUNCTION("7oxv3PPCumo", "libkernel", 1, "libkernel", sceKernelReserveVirtualRange);
    LIB_FUNCTION("BC+OG5m9+bw", "libkernel", 1, "libkernel", sceKernelGetDirectMemoryType);
    LIB_FUNCTION("pO96TwzOm5E", "libkernel", 1, "libkernel", sceKernelGetDirectMemorySize);
    LIB_FUNCTION("yDBwVAolDgg", "libkernel", 1, "libkernel", sceKernelIsStack);
    LIB_FUNCTION("jh+8XiK4LeE", "libkernel", 1, "libkernel", sceKernelIsAddressSanitizerEnabled);
    LIB_FUNCTION("NcaWUxfMNIQ", "libkernel", 1, "libkernel", sceKernelMapNamedDirectMemory);
    LIB_FUNCTION("L-Q3LEjIbgA", "libkernel", 1, "libkernel", sceKernelMapDirectMemory);
    LIB_FUNCTION("BQQniolj9tQ", "libkernel", 1, "libkernel", sceKernelMapDirectMemory2);
    LIB_FUNCTION("WFcfL2lzido", "libkernel", 1, "libkernel", sceKernelQueryMemoryProtection);
    LIB_FUNCTION("BHouLQzh0X0", "libkernel", 1, "libkernel", sceKernelDirectMemoryQuery);
    LIB_FUNCTION("MBuItvba6z8", "libkernel", 1, "libkernel", sceKernelReleaseDirectMemory);
    LIB_FUNCTION("PGhQHd-dzv8", "libkernel", 1, "libkernel", sceKernelMmap);
    LIB_FUNCTION("cQke9UuBQOk", "libkernel", 1, "libkernel", sceKernelMunmap);
    LIB_FUNCTION("mL8NDH86iQI", "libkernel", 1, "libkernel", sceKernelMapNamedFlexibleMemory);
    LIB_FUNCTION("aNz11fnnzi4", "libkernel", 1, "libkernel", sceKernelAvailableFlexibleMemorySize);
    LIB_FUNCTION("aNz11fnnzi4", "libkernel_avlfmem", 1, "libkernel",
                 sceKernelAvailableFlexibleMemorySize);
    LIB_FUNCTION("IWIBBdTHit4", "libkernel", 1, "libkernel", sceKernelMapFlexibleMemory);
    LIB_FUNCTION("p5EcQeEeJAE", "libkernel", 1, "libkernel", _sceKernelRtldSetApplicationHeapAPI);
    LIB_FUNCTION("2SKEx6bSq-4", "libkernel", 1, "libkernel", sceKernelBatchMap);
    LIB_FUNCTION("kBJzF8x4SyE", "libkernel", 1, "libkernel", sceKernelBatchMap2);
    LIB_FUNCTION("DGMG3JshrZU", "libkernel", 1, "libkernel", sceKernelSetVirtualRangeName);
    LIB_FUNCTION("n1-v6FgU7MQ", "libkernel", 1, "libkernel", sceKernelConfiguredFlexibleMemorySize);

    LIB_FUNCTION("vSMAm3cxYTY", "libkernel", 1, "libkernel", sceKernelMprotect);
    LIB_FUNCTION("YQOfxL4QfeU", "libkernel", 1, "libkernel", posix_mprotect);
    LIB_FUNCTION("YQOfxL4QfeU", "libScePosix", 1, "libkernel", posix_mprotect);
    LIB_FUNCTION("9bfdLIyuwCY", "libkernel", 1, "libkernel", sceKernelMtypeprotect);

    LIB_FUNCTION("qCSfqDILlns", "libkernel", 1, "libkernel", sceKernelMemoryPoolExpand);
    LIB_FUNCTION("pU-QydtGcGY", "libkernel", 1, "libkernel", sceKernelMemoryPoolReserve);
    LIB_FUNCTION("Vzl66WmfLvk", "libkernel", 1, "libkernel", sceKernelMemoryPoolCommit);
    LIB_FUNCTION("LXo1tpFqJGs", "libkernel", 1, "libkernel", sceKernelMemoryPoolDecommit);
    LIB_FUNCTION("YN878uKRBbE", "libkernel", 1, "libkernel", sceKernelMemoryPoolBatch);
    LIB_FUNCTION("bvD+95Q6asU", "libkernel", 1, "libkernel", sceKernelMemoryPoolGetBlockStats);

    LIB_FUNCTION("BPE9s9vQQXo", "libkernel", 1, "libkernel", posix_mmap);
    LIB_FUNCTION("BPE9s9vQQXo", "libScePosix", 1, "libkernel", posix_mmap);
    LIB_FUNCTION("UqDGjXA5yUM", "libkernel", 1, "libkernel", posix_munmap);
    LIB_FUNCTION("UqDGjXA5yUM", "libScePosix", 1, "libkernel", posix_munmap);

    LIB_FUNCTION("BohYr-F7-is", "libkernel", 1, "libkernel", sceKernelSetPrtAperture);
    LIB_FUNCTION("L0v2Go5jOuM", "libkernel", 1, "libkernel", sceKernelGetPrtAperture);
}

}
