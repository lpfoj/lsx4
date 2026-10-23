// SPDX-License-Identifier: GPL-2.0-or-later
#include "GuestThreads.h"
#include <limits>

namespace Lsx4::Apple {
namespace {
constexpr std::uint32_t Again=0x80020023,Fault=0x8002000e,Invalid=0x80020016,
    NotFound=0x80020003,Deadlock=0x8002000b,NoMemory=0x8002000c;
constexpr std::uint64_t TcbStride=0x1000000000,TlsStride=0x8000000;
constexpr std::uint64_t StackStart=0x680000000000,StackStride=0x10000000;
constexpr std::uint64_t MaxStack=64*1024*1024,ThreadMemoryLimit=64*1024*1024;
constexpr std::uint64_t AlignPage(std::uint64_t size) {return (size+0x3fff)&~0x3fffull;}
}
GuestThreads::GuestThreads(GuestAddressSpace& memory,std::vector<GuestTlsTemplate> tls)
    :memory_(memory),tls_(std::move(tls)) {}
void GuestThreads::StartMain(const Translation::CpuFrame& frame,std::uint64_t ip,
                             std::uint64_t stack,std::uint64_t stackBytes) {
    if(!threads_.empty())throw std::runtime_error("Guest scheduler already started");
    Thread thread;thread.frame=frame;thread.ip=ip;thread.handle=current_;thread.name="main";
    thread.parameters.stackAddress=stack;thread.parameters.stackBytes=stackBytes;
    threads_.emplace(current_,std::move(thread));
}
const GuestThreads::Thread* GuestThreads::Find(std::uint64_t handle) const {
    const auto found=threads_.find(handle);
    return found==threads_.end() || found->second.state==Thread::State::Reaped?nullptr:&found->second;
}
std::uint32_t GuestThreads::Create(std::uint64_t output,const GuestThreadParameters& requested,
    std::uint64_t entry,std::uint64_t argument,std::uint64_t name,const Translation::CpuFrame& parent) {
    if(!output || !memory_.CanWrite(output,8))return Fault;
    if(!entry || memory_.Fetch(entry).empty())return Fault;
    if(requested.stackBytes<16384 || requested.stackBytes>MaxStack || requested.guardBytes>MaxStack ||
       requested.suspended || requested.reserved || (requested.flags&~7u) ||
       requested.policy<1 || requested.policy>3 || (requested.inherit!=0 && requested.inherit!=4))return Invalid;
    if(next_>=128 || tls_.size()>128)return Again;
    std::string threadName;
    if(name) {
        for(unsigned i=0;;++i) {
            std::uint8_t c;
            if(name>UINT64_MAX-i || !memory_.Read(name+i,{&c,1}))return Fault;
            if(!c)break;
            if(i==32)return 0x8002003f;
            threadName+=char(c);
        }
    }
    Thread thread;thread.parameters=requested;thread.name=std::move(threadName);
    if(requested.inherit==4) {
        thread.parameters.policy=Current().parameters.policy;
        thread.parameters.priority=Current().parameters.priority;
        thread.parameters.flags=(requested.flags&~2u)|(Current().parameters.flags&2u);
    }
    // The scheduler has one virtual CPU. It does not retain an attribute object's
    // cpuset pointer after the guest is allowed to destroy that object.
    thread.parameters.cpuSet=thread.parameters.cpuSetBytes=0;
    thread.detached=(requested.flags&1)!=0;
    const auto tcb=MainTcb+next_*TcbStride;
    thread.handle=tcb+0x1000;
    auto stack=requested.stackAddress;
    const auto stackSize=requested.stackAddress?requested.stackBytes:AlignPage(requested.stackBytes);
    std::vector<GuestSegment> pending;
    auto allocation=[&](std::uint64_t address,std::uint64_t bytes) {
        if(!memory_.IsFree(address,bytes))throw std::runtime_error("Guest thread mapping collision");
        for(const auto& segment:pending)
            if(address<segment.address+segment.bytes.size() && segment.address<address+bytes)
                throw std::runtime_error("Overlapping thread TLS mappings");
        pending.push_back({address,6,std::vector<std::uint8_t>(bytes)});
    };
    // Validate everything before modifying a user-supplied stack or publishing a
    // handle. Every successfully mapped region is tracked for rollback/reaping.
    if(stack && (stack>UINT64_MAX-stackSize || !memory_.CanWrite(stack,stackSize) ||
                 !memory_.CanRead(stack,stackSize)))return Fault;
    std::uint64_t allocationBytes=requested.stackAddress?0:stackSize;
    allocationBytes+=0x4000;
    for(const auto& tls:tls_)if(tls.size) {
        const auto align=std::max<std::uint64_t>(tls.alignment,32);
        if((align&(align-1)) || align>TlsStride || tls.size>TlsStride-align || tls.initial.size()>tls.size)return Invalid;
        allocationBytes+=(tls.size+align-1)&~(align-1);
    }
    // This is the scheduler's own allocation budget. DirectMemory has a separate
    // budget; counting its mapped game assets as thread storage would reject the
    // first small worker after a legitimate graphics allocation.
    std::uint64_t ownedBytes=0;
    for(const auto& [handle,existing]:threads_)
        for(const auto& [address,bytes]:existing.allocations)ownedBytes+=bytes;
    if(allocationBytes>ThreadMemoryLimit || ownedBytes>ThreadMemoryLimit-allocationBytes)return NoMemory;
    try {
        if(!stack) {
            stack=StackStart+next_*StackStride+AlignPage(requested.guardBytes);
            // Guard bytes remain unmapped. They consume virtual space only.
            if(requested.guardBytes && !memory_.IsFree(stack-AlignPage(requested.guardBytes),AlignPage(requested.guardBytes)))return Again;
            allocation(stack,stackSize);
        }
        allocation(tcb,0x4000);
        std::vector<std::uint64_t> tlsAddresses(tls_.size());
        for(std::size_t i=0;i<tls_.size();++i)if(tls_[i].size) {
            const auto align=std::max<std::uint64_t>(tls_[i].alignment,32);
            const auto size=(tls_[i].size+align-1)&~(align-1);
            const auto address=i?tcb+i*TlsStride:tcb-size;
            allocation(address,size);
            std::copy(tls_[i].initial.begin(),tls_[i].initial.end(),pending.back().bytes.begin());
            tlsAddresses[i]=address;
        }
        thread.allocations.reserve(pending.size());
        for(auto& segment:pending) {
            const auto address=segment.address;const auto size=segment.bytes.size();
            memory_.Map(std::move(segment));thread.allocations.emplace_back(address,size);
        }
        memory_.Put(tcb,tcb);memory_.Put(tcb+8,tcb+0x100);memory_.Put(tcb+16,thread.handle);
        memory_.Put(tcb+0x100,1);memory_.Put(tcb+0x108,tls_.size());
        for(std::size_t i=0;i<tlsAddresses.size();++i)memory_.Put(tcb+0x110+i*8,tlsAddresses[i]);
        thread.parameters.stackAddress=stack;thread.parameters.stackBytes=stackSize;
        thread.frame.fs_origin=tcb;thread.frame.condition_word=2;
        thread.frame.simd_control=parent.simd_control;thread.frame.x87_control=parent.x87_control;
        thread.frame.integer[7]=argument;thread.frame.integer[4]=((stack+stackSize)&~15ull)-8;
        thread.ip=entry;
        const auto handle=thread.handle,rsp=thread.frame.integer[4];
        threads_.emplace(handle,std::move(thread));
        memory_.Put(rsp,ThreadReturn);memory_.Put(output,handle);
        ++next_;
        return 0;
    } catch(const std::bad_alloc&) {
        Release(thread);return NoMemory;
    } catch(...) {
        Release(thread);throw;
    }
}
void GuestThreads::Release(Thread& thread) {
    for(const auto& [address,bytes]:thread.allocations)memory_.Unmap(address,bytes);
    thread.allocations.clear();
}
GuestThreadResult GuestThreads::Join(std::uint64_t target,std::uint64_t output) {
    if(target==current_)return {Deadlock};
    if(!Find(target))return {NotFound};
    auto& thread=threads_.at(target);
    if(thread.detached || (thread.joiner && thread.joiner!=current_))return {Invalid};
    if(output && !memory_.CanWrite(output,8))return {Fault};
    if(thread.state!=Thread::State::Exited) {thread.joiner=current_;return {0,true};}
    if(output)memory_.Put(output,thread.result);
    thread.joiner=0;thread.state=Thread::State::Reaped;Release(thread);return {0};
}
std::uint32_t GuestThreads::Detach(std::uint64_t target) {
    if(!Find(target))return NotFound;
    auto& thread=threads_.at(target);
    if(thread.detached || thread.joiner)return Invalid;
    thread.detached=true;thread.parameters.flags|=1;
    if(thread.state==Thread::State::Exited) {thread.state=Thread::State::Reaped;Release(thread);}
    return 0;
}
void GuestThreads::Exit(std::uint64_t value) {
    auto& thread=Current();thread.result=value;thread.state=Thread::State::Exited;thread.wake={};
    if(thread.detached) {thread.state=Thread::State::Reaped;Release(thread);}
}
void GuestThreads::Block(std::function<bool(Thread&)> wake) {
    auto& thread=Current();thread.state=Thread::State::Blocked;thread.wake=std::move(wake);
}
bool GuestThreads::Switch(Translation::CpuFrame& frame,std::uint64_t& ip) {
    auto& old=Current();old.frame=frame;old.ip=ip;
    const auto previous=current_;
    // Wake callbacks may perform an operation on behalf of a blocked caller;
    // Current() must identify that caller (not the thread which released a lock).
    for(auto& [handle,thread]:threads_)if(thread.state==Thread::State::Blocked && thread.wake) {
        current_=handle;
        if(thread.wake(thread)) {thread.state=Thread::State::Runnable;thread.wake={};}
    }
    current_=previous;
    auto next=threads_.upper_bound(previous);
    for(std::size_t i=0;i<threads_.size();++i) {
        if(next==threads_.end())next=threads_.begin();
        if(next->second.state==Thread::State::Runnable) {
            current_=next->first;frame=next->second.frame;ip=next->second.ip;
            switches_+=current_!=previous;return true;
        }
        ++next;
    }
    return false;
}
bool GuestThreads::AllExited() const {
    for(const auto& [handle,thread]:threads_)
        if(thread.state==Thread::State::Runnable || thread.state==Thread::State::Blocked)return false;
    return true;
}
}
