// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <execinfo.h>
#include <mach/mach.h>
#include <mach-o/dyld.h>
#include <memory_resource>
#include <new>
#include <sys/mman.h>
#include <unistd.h>

static std::uintptr_t allocationDiagnosticImageBase{};
// Observe failed upstream requests, including the size of a monotonic pool's
// next chunk. Forward successful allocations unchanged; enabled only by the
// explicit fixture switch below.
class AppleAllocationDiagnosticResource final : public std::pmr::memory_resource {
    std::atomic<unsigned> failures_{};
    void* do_allocate(std::size_t bytes,std::size_t alignment) override {
        try { return std::pmr::new_delete_resource()->allocate(bytes,alignment); }
        catch(...) {
            if(failures_.fetch_add(1,std::memory_order_relaxed)>=4) throw;
            // A temporary owned reservation distinguishes a failed malloc
            // request from exhaustion of the same-sized anonymous VM range.
            void* probe=MAP_FAILED;int mapError=0;
            if(bytes && bytes<=64ULL<<20) {
                probe=mmap(nullptr,bytes,PROT_NONE,MAP_PRIVATE|MAP_ANON,-1,0);
                if(probe==MAP_FAILED) mapError=errno;
                else munmap(probe,bytes);
            }
            char line[192];
            const int count=std::snprintf(line,sizeof(line),
                "[APPLE_PMR_ALLOCATION_FAILURE] bytes=%zu alignment=%zu vmProbe=%p errno=%d\n",
                bytes,alignment,probe,mapError);
            if(count>0) (void)::write(STDERR_FILENO,line,std::min<std::size_t>(count,sizeof(line)-1));
            throw;
        }
    }
    void do_deallocate(void* p,std::size_t bytes,std::size_t alignment) override {
        std::pmr::new_delete_resource()->deallocate(p,bytes,alignment);
    }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this==&other;
    }
};

static void ReportAppleAllocationRegions() {
    vm_address_t address=0;
    vm_address_t previousEnd=1ULL<<32,largestHoleAddress=0;
    vm_size_t largestHole=0,totalHoles=0;
    natural_t depth=0;
    for(unsigned regions=0;regions<4096;++regions) {
        vm_size_t size=0;
        vm_region_submap_info_data_64_t info{};
        mach_msg_type_number_t count=VM_REGION_SUBMAP_INFO_COUNT_64;
        if(vm_region_recurse_64(mach_task_self(),&address,&size,&depth,
                reinterpret_cast<vm_region_recurse_info_t>(&info),&count)!=KERN_SUCCESS) break;
        if(info.is_submap) { ++depth;continue; }
        if(address>previousEnd) {
            const auto hole=address-previousEnd;
            totalHoles+=hole;
            if(hole>largestHole) {largestHole=hole;largestHoleAddress=previousEnd;}
        }
        if(size>=32ULL<<20) {
            char line[192];
            const int length=std::snprintf(line,sizeof(line),
                "[APPLE_ALLOCATION_VM] address=0x%lx bytes=%lu tag=%u protection=%u\n",
                address,size,info.user_tag,info.protection);
            if(length>0) (void)::write(STDERR_FILENO,line,std::min<std::size_t>(length,sizeof(line)-1));
        }
        if(!size || address+size<address) break;
        address+=size;
        previousEnd=std::max(previousEnd,address);
    }
    char line[224];
    const int length=std::snprintf(line,sizeof(line),
        "[APPLE_ALLOCATION_VM_HOLES] scannedThrough=0x%lx totalBytes=%lu largestAddress=0x%lx largestBytes=%lu\n",
        address,totalHoles,largestHoleAddress,largestHole);
    if(length>0) (void)::write(STDERR_FILENO,line,std::min<std::size_t>(length,sizeof(line)-1));
}
static void InstallAppleAllocationFailureDiagnostics() {
    if(!std::getenv("LSX4_PROBE_ALLOCATION_FAILURE_TRACE")) return;
    allocationDiagnosticImageBase=reinterpret_cast<std::uintptr_t>(_dyld_get_image_header(0));
    void* warmup[32];(void)backtrace(warmup,32);
    static AppleAllocationDiagnosticResource resource;
    std::pmr::set_default_resource(&resource);
    std::set_new_handler(+[] {
        static std::atomic<unsigned> failures{};
        const auto failure=failures.fetch_add(1,std::memory_order_relaxed);
        if(failure<4) {
            void* frames[32];const int count=backtrace(frames,32);
            task_vm_info_data_t memory{};mach_msg_type_number_t length=TASK_VM_INFO_COUNT;
            task_info(mach_task_self(),TASK_VM_INFO,reinterpret_cast<task_info_t>(&memory),&length);
            char line[256];
            int used=std::snprintf(line,sizeof(line),"[APPLE_ALLOCATION_FAILURE] image=0x%llx footprint=%llu virtual=%llu limitBytesRemaining=%llu frames=%d\n",
                static_cast<unsigned long long>(allocationDiagnosticImageBase),memory.phys_footprint,memory.virtual_size,memory.limit_bytes_remaining,count);
            if(used>0) (void)::write(STDERR_FILENO,line,std::min<std::size_t>(used,sizeof(line)-1));
            for(int i=0;i<count;++i) {
                used=std::snprintf(line,sizeof(line),"[APPLE_ALLOCATION_FRAME] index=%d address=%p\n",i,frames[i]);
                if(used>0) (void)::write(STDERR_FILENO,line,std::min<std::size_t>(used,sizeof(line)-1));
            }
            if(failure==0) ReportAppleAllocationRegions();
        }
        throw std::bad_alloc{};
    });
}

// Current owns process-lifetime monotonic pools. Install the optional observer
// before those pools capture their upstream resource during static startup.
__attribute__((constructor)) static void InstallEarlyAppleAllocationDiagnostics() {
    InstallAppleAllocationFailureDiagnostics();
}
