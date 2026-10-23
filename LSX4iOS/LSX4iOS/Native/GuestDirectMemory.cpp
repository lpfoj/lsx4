// SPDX-License-Identifier: GPL-2.0-or-later
#include "GuestDirectMemory.h"
#include <bit>
namespace Lsx4::Apple {
namespace {
constexpr std::uint64_t Page=16384,MiB=1024*1024,ResidentLimit=512*MiB;
constexpr std::uint32_t Invalid=0x80020016,Again=0x80020023,NoMemory=0x8002000c,Busy=0x80020010;
std::uint64_t Align(std::uint64_t value,std::uint64_t alignment) {
    if(value>UINT64_MAX-alignment+1)throw std::runtime_error("Guest alignment overflow");
    return ((value+alignment-1)/alignment)*alignment;
}
}
void GuestDirectMemory::Configure(std::uint64_t parameters,GuestAddressSpace& memory) {
    // Same base-PS4 budget calculation as Linker::Execute/SetupMemoryRegions.
    if(!parameters)return;
    const auto parameterSize=memory.Get(parameters);
    if(parameterSize<24)return;
    sdk_=std::uint32_t(memory.Get(parameters+16));
    std::uint64_t flexible=512*MiB;bool extended=sdk_>=0x05000000;
    if(parameterSize>=72)if(const auto pointer=memory.Get(parameters+64)) {
        const auto bytes=memory.Get(pointer);
        if(bytes>=24)if(const auto sizePointer=memory.Get(pointer+16)) {
            flexible=memory.Get(sizePointer);
            if(flexible>5248*MiB-64*MiB)throw std::runtime_error("Invalid guest flexible memory budget");
            flexible+=64*MiB;
        }
        if(!extended && bytes>=48)if(const auto flag=memory.Get(pointer+40)) {
            std::uint8_t value{};if(!memory.Read(flag,{&value,1}))throw std::runtime_error("Invalid extended memory flag");
            extended=value!=0;
        }
    }
    const auto total=(extended?5248ull:5120ull)*MiB;
    if(flexible>total)throw std::runtime_error("Guest memory budget exceeds machine profile");
    size_=total-flexible;
}
std::optional<std::uint64_t> GuestDirectMemory::Call(std::string_view identity,
    const Translation::CpuFrame& frame,GuestAddressSpace& memory) {
    const auto a=frame.integer[7],b=frame.integer[6],c=frame.integer[2],d=frame.integer[1],e=frame.integer[8],f=frame.integer[9];
    if(identity=="usHTMoFoBTM#libkernel_dmem_aliasing2#libkernel" || identity=="usHTMoFoBTM#libkernel#libkernel") {
        aliasing_=true;return 0;
    }
    constexpr std::string_view suffix="#libkernel#libkernel";
    if(!identity.ends_with(suffix))return std::nullopt;
    const auto nid=identity.substr(0,identity.size()-suffix.size());
    if(nid=="pO96TwzOm5E")return size_;
    if(nid=="WB66evu8bsU") {
        if(!a || !memory.CanWrite(a,4))return Invalid;
        memory.Put32(a,sdk_);return 0;
    }
    if(nid=="rTXw65xmLIA" || nid=="B+vc2AO2Zrc") {
        const bool main=nid=="B+vc2AO2Zrc";
        const auto start=main?0:a,end=main?size_:b,length=main?a:c,alignment=main?b:d,output=main?d:f;
        const auto type=std::uint32_t(main?c:e);
        if(start>INT64_MAX || end>INT64_MAX || !length || length%Page ||
           (alignment && alignment%Page) || alignment>INT64_MAX || type>10 || !output || !memory.CanWrite(output,8))return Invalid;
        if(end<=start || end>size_ || length>end-start)return Again;
        const auto align=alignment?alignment:Page;auto candidate=Align(start,align);
        for(const auto& [base,block]:blocks_) {
            if(base+block.size<=candidate)continue;
            if(candidate<=base && length<=base-candidate)break;
            candidate=Align(base+block.size,align);
        }
        if(candidate>end || length>end-candidate)return Again;
        std::set<const void*> seen;std::uint64_t resident=0;
        for(const auto& [base,block]:blocks_)if(seen.insert(block.storage.get()).second)resident+=block.storage->size();
        if(length>ResidentLimit || resident>ResidentLimit-length)return NoMemory;
        try {
            auto storage=std::make_shared<std::vector<std::uint8_t>>(length);
            blocks_.emplace(candidate,Block{length,0,type,std::move(storage)});
        }catch(const std::bad_alloc&){return NoMemory;}
        memory.Put(output,candidate);return 0;
    }
    if(nid=="NcaWUxfMNIQ" || nid=="L-Q3LEjIbgA") {
        if(!a || !memory.CanRead(a,8) || !memory.CanWrite(a,8) || !b || b%Page || e%Page || e>INT64_MAX || f>INT64_MAX)return Invalid;
        if(f && !std::has_single_bit(f) && f%Page)return Invalid;
        if(c&4)return 0x8002000d; // Direct mappings cannot execute CPU code.
        if(c&~0x33ull)return Invalid;
        if(d&0x100)return std::nullopt; // Virtual reservations need their own lifecycle.
        if(d&~0x421c93ull)return Invalid;
        if(nid=="NcaWUxfMNIQ") {
            const auto name=memory.Get(frame.integer[4]+8);if(!name)return Invalid;
            bool terminated=false;
            for(unsigned i=0;i<32;++i) {
                std::uint8_t value{};
                if(name>UINT64_MAX-i || !memory.Read(name+i,{&value,1}))return Invalid;
                if(!value){terminated=true;break;}
            }
            if(!terminated)return 0x8002003f;
        }
        auto found=blocks_.upper_bound(e);if(found==blocks_.begin())return NoMemory;--found;
        const auto& block=found->second;const auto relative=e-found->first;
        if(relative>block.size || b>block.size-relative)return NoMemory;
        const auto offset=block.offset+relative;
        if(!aliasing_ && sdk_>=0x02500000 && !(d&0x400) && memory.StorageMapped(block.storage,offset,b))return Busy;
        auto address=memory.Get(a);const auto alignment=std::max<std::uint64_t>(f,Page);
        const bool fixed=d&0x10,noOverwrite=d&0x80;
        if((fixed || noOverwrite) && (!address || address%Page))return Invalid;
        if(address>UINT64_MAX-b)return Invalid;
        if(noOverwrite && !memory.IsFree(address,b))return 0x80020011;
        if(!fixed && (!address || address%alignment || !memory.IsFree(address,b))) {
            address=Align(nextVirtual_,alignment);
            while(!memory.IsFree(address,b)) {
                if(address>0x4000000000-b)return NoMemory;
                address=Align(address+Page,alignment);
            }
        }
        // The common MemoryManager::CreateArea normalizes CpuWrite to include
        // CpuRead, matching x86 page permissions. GPU-only flags stay separate.
        try {memory.MapShared(address,(c&3?4:0)|(c&2)|(c&0x30),block.storage,offset,b,fixed);}
        catch(const std::bad_alloc&){return NoMemory;}
        memory.Put(a,address);nextVirtual_=std::max(nextVirtual_,address+b);return 0;
    }
    if(nid=="MBuItvba6z8" || nid=="hwVSPCmp5tM") {
        if(a%Page || b%Page || a>size_ || b>size_-a)return Invalid;
        if(!b)return 0;
        auto updated=blocks_;std::uint64_t covered=0;
        for(const auto& [base,block]:blocks_) {
            const auto start=std::max(base,a),end=std::min(base+block.size,a+b);
            if(start>=end)continue;
            covered+=end-start;updated.erase(base);
            if(base<start)updated.emplace(base,Block{start-base,block.offset,block.type,block.storage});
            if(end<base+block.size)updated.emplace(end,Block{base+block.size-end,block.offset+end-base,block.type,block.storage});
        }
        if(covered!=b && nid=="hwVSPCmp5tM")return Invalid;
        for(const auto& [base,block]:blocks_) {
            const auto start=std::max(base,a),end=std::min(base+block.size,a+b);
            if(start<end)memory.UnmapStorage(block.storage,block.offset+start-base,end-start);
        }
        blocks_=std::move(updated);return 0;
    }
    return std::nullopt;
}
}
