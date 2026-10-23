// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
// Exercise exhaustion of the primary arena through public common memory APIs.
// Reservations are PROT_NONE; only two pages of actual physical backing are touched.
namespace FragmentedGuestArenaFixture {
static void TestAdjacentReservation(Core::MemoryManager* memory) {
    constexpr u64 page=16ULL<<10;
    auto* window=static_cast<u8*>(mmap(nullptr,3*page,PROT_READ|PROT_WRITE,
                                      MAP_PRIVATE|MAP_ANON,-1,0));
    Require(window!=MAP_FAILED,"Cannot allocate adjacent reservation fixture window");
    struct Ends {
        u8* window;
        ~Ends() { munmap(window,page);munmap(window+2*page,page); }
    } ends{window};
    constexpr u64 marker=0x86d4a932b7e51c60ULL;
    *reinterpret_cast<u64*>(window)=marker;
    *reinterpret_cast<u64*>(window+2*page)=marker;
    Require(munmap(window+page,page)==0,"Cannot release adjacent fixture hole");
    auto& space=memory->GetAddressSpace();
    const auto first=reinterpret_cast<VAddr>(window);
    const auto middle=first+page;
    // Only the new middle page becomes owned by AddressSpace. The live pages
    // on either side must survive both a successful hint and a rejected hint.
    Require(space.ReserveAdditionalRange(page,page,middle)==middle &&
            space.ContainsOwnedRange(middle,page),"Adjacent host hole was not reserved exactly");
    Require(space.ReserveAdditionalRange(page,page,first)==0 &&
            !space.ContainsOwnedRange(first,page),"An occupied host hint became guest memory");
    Require(*reinterpret_cast<u64*>(window)==marker &&
            *reinterpret_cast<u64*>(window+2*page)==marker,
            "Adjacent reservation changed an existing host mapping");
    std::fprintf(stderr,"[APPLE_ADJACENT_ARENA_TEST] exactHole=1 occupiedHintRejected=1 hostCanaries=1\n");
}
static void Run(Core::MemoryManager* memory,VAddr heap,u64 heapBytes) {
    constexpr u64 page=16ULL<<10,alignment=2ULL<<20;
    const auto primaryFirst=memory->SystemReservedVirtualBase()-(512ULL<<20);
    const auto primaryEnd=heap+heapBytes;
    struct Reservations {
        Core::MemoryManager* memory;
        std::vector<std::pair<VAddr,u64>> entries;
        ~Reservations() { for(auto [address,bytes]:entries) memory->UnmapMemory(address,bytes); }
    } reserved{memory,{}};
    VAddr cursor=primaryFirst;
    while(cursor<primaryEnd) {
        Libraries::Kernel::OrbisVirtualQueryInfo info{};
        const bool found=memory->VirtualQuery(cursor,1,&info)==0 && info.start<primaryEnd;
        const auto end=found?info.start:primaryEnd;
        if(end>cursor) {
            void* address{};
            Require(memory->MapMemory(&address,cursor,end-cursor,Core::MemoryProt::NoAccess,
                Core::MemoryMapFlags::Fixed|Core::MemoryMapFlags::NoOverwrite,
                Core::VMAType::Reserved,"spill-fixture-fill",false)==0,
                "Cannot reserve primary free VMA for exhaustion fixture");
            reserved.entries.emplace_back(cursor,end-cursor);
        }
        if(!found) break;
        Require(info.end>cursor,"VirtualQuery did not advance across the primary arena");
        cursor=info.end;
    }
    const auto physical=memory->Allocate(0,memory->GetTotalDirectSize(),page,page,3);
    Require(physical!=static_cast<PAddr>(-1),"Spill fixture physical allocation failed");
    void* first{};void* alias{};
    Require(memory->MapMemory(&first,Core::DEFAULT_MAPPING_BASE,page,Core::MemoryProt::CpuReadWrite,
        Core::MemoryMapFlags{},Core::VMAType::Direct,"spill-primary",true,physical,alignment)==0,
        "Primary VMA exhaustion rejected an available secondary host range");
    const auto address=reinterpret_cast<VAddr>(first);
    Require(address%alignment==0 && (address<primaryFirst || address>=primaryEnd),
        "Spill allocation was not aligned or replaced primary ownership");
    Require(memory->MapMemory(&alias,Core::DEFAULT_MAPPING_BASE,page,Core::MemoryProt::CpuReadWrite,
        Core::MemoryMapFlags{},Core::VMAType::Direct,"spill-alias",false,physical,page)==0,
        "Secondary range physical alias failed");
    constexpr u64 marker=0x6192a8b5c3d4e70fULL;
    Executor::Jit::StoreGuestScalar(address,marker,8);
    Require(Executor::Jit::LoadGuestScalar(reinterpret_cast<VAddr>(alias),8)==marker,
        "Secondary guest range lost JIT ownership or physical aliasing");
    Libraries::Kernel::OrbisVirtualQueryInfo info{};
    Require(memory->VirtualQuery(address,0,&info)==0 && info.start==address && info.end==address+page,
        "VirtualQuery clipped the secondary range to the primary arena");
    void* host=mmap(nullptr,page,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);
    Require(host!=MAP_FAILED,"Spill fixture host canary allocation failed");
    *static_cast<u64*>(host)=marker;void* rejected{};
    const auto rejectedResult=memory->MapMemory(&rejected,reinterpret_cast<VAddr>(host),page,
        Core::MemoryProt::CpuReadWrite,Core::MemoryMapFlags::Fixed|Core::MemoryMapFlags::NoOverwrite,
        Core::VMAType::Direct,"spill-unowned",false,physical,page);
    const bool untouched=*static_cast<u64*>(host)==marker;
    munmap(host,page);
    Require(rejectedResult!=0 && untouched,"Additional arena registration weakened fixed ownership");
    const auto aliasAddress=reinterpret_cast<VAddr>(alias);
    Require(memory->UnmapMemory(address,page)==0 && memory->UnmapMemory(aliasAddress,page)==0,
        "Secondary guest range unmap failed");
    void* reused{};
    Require(memory->MapMemory(&reused,primaryFirst,page,Core::MemoryProt::CpuReadWrite,
        Core::MemoryMapFlags{},Core::VMAType::Direct,"spill-reuse",false,physical,page)==0 &&
        (reinterpret_cast<VAddr>(reused)==address || reinterpret_cast<VAddr>(reused)==aliasAddress),
        "SearchFree skipped a reusable secondary VMA below the advisory address");
    Require(memory->Free(physical,page,true)==0,"Secondary physical alias cleanup failed");
    Require(JitMemoryRouteFixture::Rejects([&]{(void)Executor::Jit::LoadGuestScalar(
        reinterpret_cast<VAddr>(reused),8);}),"Freed secondary backing retained data access");
    std::fprintf(stderr,"[APPLE_FRAGMENTED_ARENA_TEST] sparsePrimaryExhaustion=1 additionalAligned=1 alias=1 query=1 fixedUnownedRejected=1 reused=1 physicalFree=1 base=0x%llx\n",
        static_cast<unsigned long long>(address));
    TestAdjacentReservation(memory);
}
}
