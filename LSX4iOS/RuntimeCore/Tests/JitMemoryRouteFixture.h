// SPDX-License-Identifier: GPL-2.0-or-later
// Exercise the public JIT helpers through real Core::Memory ownership checks.
#pragma once
#include "executor/dynamic_translation/retiring_execution_core.h"
#include "executor/dynamic_translation/process_memory.h"
#include <csignal>
#include <sys/mman.h>
#include <unistd.h>

namespace JitMemoryRouteFixture {
static void* watchedPage{};
static std::size_t pageBytes{};
static volatile sig_atomic_t handled{};
static void HandleAccess(int, siginfo_t* info, void*) {
    const auto address=reinterpret_cast<std::uintptr_t>(info->si_addr);
    const auto first=reinterpret_cast<std::uintptr_t>(watchedPage);
    if(address<first || address-first>=pageBytes ||
       mprotect(watchedPage,pageBytes,PROT_READ|PROT_WRITE)!=0) _exit(97);
    handled=handled+1;
}
template<class Operation> static bool Rejects(Operation operation) {
    try { operation(); } catch(const std::runtime_error&) { return true; }
    return false;
}
static void Run(Core::MemoryManager* memory,void* first,void* alias,std::size_t bytes) {
    struct Handlers {
        struct sigaction oldBus{},oldSegv{};
        Handlers(void* page,std::size_t bytes) {
            watchedPage=page;pageBytes=bytes;handled=0;
            struct sigaction action{};action.sa_sigaction=HandleAccess;action.sa_flags=SA_SIGINFO;
            sigemptyset(&action.sa_mask);
            Require(sigaction(SIGBUS,&action,&oldBus)==0,"JIT fixture SIGBUS setup failed");
            if(sigaction(SIGSEGV,&action,&oldSegv)!=0) {
                sigaction(SIGBUS,&oldBus,nullptr);
                throw std::runtime_error("JIT fixture SIGSEGV setup failed");
            }
        }
        ~Handlers() {
            mprotect(watchedPage,pageBytes,PROT_READ|PROT_WRITE);
            sigaction(SIGBUS,&oldBus,nullptr);sigaction(SIGSEGV,&oldSegv,nullptr);
        }
    } handlers(first,bytes);
    const auto address=reinterpret_cast<VAddr>(first);
    const u64 original=*static_cast<u64*>(alias);
    constexpr u64 marker=0x198ba2475c6d3e0fULL,canary=0x71539ac02468def0ULL;
    std::memcpy(static_cast<unsigned char*>(alias)+8,&canary,8);
    Require(mprotect(first,bytes,PROT_READ)==0,"JIT write watcher setup failed");
    Executor::Jit::StoreGuestScalar(address,marker,8);
    Require(handled==1 && *static_cast<u64*>(alias)==marker &&
            static_cast<u64*>(alias)[1]==canary,"JIT store bypassed the page owner or changed adjacent bytes");
    Require(mprotect(first,bytes,PROT_NONE)==0,"JIT read watcher setup failed");
    u64 inspected{};
    Require(!Lsx4::Translation::ReadProcessGuestMemory(address,&inspected,8) && handled==1,
            "Host inspection dispatched a guest page fault");
    Require(Executor::Jit::LoadGuestScalar(address,8)==marker && handled==2,
            "JIT load bypassed the page owner");
    Require(memory->Protect(address,bytes,Core::MemoryProt::CpuRead)==0,
            "JIT logical read-only setup failed");
    Require(Rejects([&]{Executor::Jit::StoreGuestScalar(address,0,8);}) && handled==2 &&
            *static_cast<u64*>(alias)==marker,"JIT store accepted a logically read-only VMA");
    Require(memory->Protect(address,bytes,Core::MemoryProt::CpuReadWrite)==0,
            "JIT logical permissions restore failed");
    // Aliases can be adjacent, so their boundary alone is a valid readable range.
    // Create an explicit logical guard to test complete-range permission checks.
    void* boundary{};
    Require(memory->MapMemory(&boundary,Core::DEFAULT_MAPPING_BASE,2*bytes,
            Core::MemoryProt::CpuReadWrite,Core::MemoryMapFlags{},Core::VMAType::Flexible,
            "jit-range-guard",false)==0,"JIT range fixture mapping failed");
    const auto boundaryAddress=reinterpret_cast<VAddr>(boundary);
    Require(memory->Protect(boundaryAddress+bytes,bytes,Core::MemoryProt::NoAccess)==0,
            "JIT range guard protection failed");
    const bool rejectedBoundaryRead=Rejects([&]{
        (void)Executor::Jit::LoadGuestScalar(boundaryAddress+bytes-4,8);
    });
    const bool rejectedBoundaryWrite=Rejects([&]{
        Executor::Jit::StoreGuestScalar(boundaryAddress+bytes-4,marker,8);
    });
    Require(memory->UnmapMemory(boundaryAddress,2*bytes)==0,"JIT range fixture unmap failed");
    Require(rejectedBoundaryRead && rejectedBoundaryWrite,
            "JIT helpers accepted a range crossing into an inaccessible VMA");
    void* unowned=mmap(nullptr,bytes,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);
    Require(unowned!=MAP_FAILED,"JIT unowned fixture allocation failed");
    std::memcpy(unowned,&marker,8);
    const auto unownedAddress=reinterpret_cast<VAddr>(unowned);
    const bool rejectedRead=Rejects([&]{(void)Executor::Jit::LoadGuestScalar(unownedAddress,8);});
    const bool rejectedWrite=Rejects([&]{Executor::Jit::StoreGuestScalar(unownedAddress,0,8);});
    const bool untouched=*static_cast<u64*>(unowned)==marker;
    // Code inspection also reads generated HLE stubs outside guest VMAs.
    // Decoding those bytes must not grant data-access authority to the mapping.
    const unsigned char stub[]={0x31,0xc0,0xc3}; // xor eax,eax; ret
    std::memset(unowned,0xcc,16);std::memcpy(unowned,stub,sizeof(stub));
    const auto decoded=Executor::Jit::DecodeGuestBasicRegion(unownedAddress,2);
    const bool decodedStub=decoded.instructions.size()==2 &&
                           decoded.end_rip==unownedAddress+sizeof(stub);
    munmap(unowned,bytes);
    Require(rejectedRead && rejectedWrite && untouched,"JIT helpers accepted unowned host memory");
    Require(decodedStub,"Non-faulting code inspection rejected a generated HLE stub");
    std::memcpy(alias,&original,8);
    std::fprintf(stderr,"[APPLE_JIT_MEMORY_ROUTE_TEST] watchedRead=1 watchedWrite=1 hostInspectionNonFaulting=1 readOnlyRejected=1 boundaryRejected=1 unownedRejected=1 handled=%d\n",handled);
}
}
