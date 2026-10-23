// SPDX-License-Identifier: GPL-2.0-or-later
#include "GuestThreadAttributes.h"
#include "core/libraries/kernel/posix_error.h"
#include <bit>

namespace Lsx4::Apple {
namespace {
static_assert(std::endian::native==std::endian::little);
template<class T> bool Read(const Translation::GuestMemoryPort& p,std::uint64_t a,T& out) {
    return a && p.read && p.read(a,{reinterpret_cast<std::uint8_t*>(&out),sizeof(out)},p.context);
}
template<class T> bool Write(const Translation::GuestMemoryPort& p,std::uint64_t a,const T& in) {
    return a && p.write && p.write(a,{reinterpret_cast<const std::uint8_t*>(&in),sizeof(in)},p.context);
}
std::uint32_t Error(unsigned code) {return code?0x80020000u+code:0;}
}
std::optional<GuestThreadParameters> GuestThreadAttributes::Get(std::uint64_t cell,
    const Translation::GuestMemoryPort& memory) const {
    if(!cell) return GuestThreadParameters{};
    std::uint64_t handle{};GuestThreadParameters value;
    if(!Read(memory,cell,handle) || !handles_.contains(handle) || !Read(memory,handle,value)) return std::nullopt;
    return value;
}
bool GuestThreadAttributes::Set(std::uint64_t cell,const GuestThreadParameters& value,
    const Translation::GuestMemoryPort& memory) {
    std::uint64_t handle{};
    return Read(memory,cell,handle) && handles_.contains(handle) && Write(memory,handle,value);
}
std::optional<std::uint32_t> GuestThreadAttributes::Call(std::string_view identity,std::uint64_t first,
    std::uint64_t second,std::uint64_t third,const Translation::GuestMemoryPort& memory) {
    const auto split=identity.find('#');
    if(split==std::string_view::npos || identity.substr(split)!="#libkernel#libkernel") return std::nullopt;
    const auto nid=identity.substr(0,split);
    enum Op {Init,Destroy,StackSize,Stack,StackAddress,Guard,Detached,Policy,Priority,Inherit,Scope,
             GetStackSize,GetGuard,GetDetached,GetPriority,GetPolicy,GetInherit,GetStackAddress,GetAffinity,SetAffinity};
    Op operation;
    if(nid=="nsYoNRywwNg") operation=Init;
    else if(nid=="62KCwEMmzcM") operation=Destroy;
    else if(nid=="UTXzJbWhhTE") operation=StackSize;
    else if(nid=="Bvn74vj6oLo") operation=Stack;
    else if(nid=="F+yfmduIBB8") operation=StackAddress;
    else if(nid=="El+cQ20DynU") operation=Guard;
    else if(nid=="-Wreprtu0Qs") operation=Detached;
    else if(nid=="4+h9EzwKF4I") operation=Policy;
    else if(nid=="DzES9hQF4f4") operation=Priority;
    else if(nid=="eXbUSpEaTsA") operation=Inherit;
    else if(nid=="YdZfEZfRnPk") operation=Scope;
    else if(nid=="-fA+7ZlGDQs") operation=GetStackSize;
    else if(nid=="txHtngJ+eyc") operation=GetGuard;
    else if(nid=="JaRMy+QcpeU") operation=GetDetached;
    else if(nid=="FXPWHNk8Of0") operation=GetPriority;
    else if(nid=="NMyIQ9WgWbU") operation=GetPolicy;
    else if(nid=="lpMP8HhkBbg") operation=GetInherit;
    else if(nid=="Ru36fiTtJzA") operation=GetStackAddress;
    else if(nid=="8+s5BzZjxSg") operation=GetAffinity;
    else if(nid=="3qxgM4ezETA") operation=SetAffinity;
    else return std::nullopt;
    if(!first) return Error(POSIX_EINVAL);
    if(operation==Init) {
        if(next_>=StorageBase+StorageBytes) return Error(POSIX_ENOMEM);
        const auto handle=next_;next_+=128;
        if(!Write(memory,handle,GuestThreadParameters{}) || !Write(memory,first,handle)) return Error(POSIX_EFAULT);
        handles_.insert(handle);return 0;
    }
    auto value=Get(first,memory);if(!value) return Error(POSIX_EINVAL);
    std::uint64_t handle{};if(!Read(memory,first,handle)) return Error(POSIX_EFAULT);
    if(operation==Destroy) {
        if(!Write(memory,first,std::uint64_t{0})) return Error(POSIX_EFAULT);
        handles_.erase(handle);return 0;
    }
    switch(operation) {
    case StackSize:if(second<16384)return Error(POSIX_EINVAL);value->stackBytes=second;break;
    case Stack:if(!second || third<16384)return Error(POSIX_EINVAL);value->stackAddress=second;value->stackBytes=third;break;
    case StackAddress:if(!second)return Error(POSIX_EINVAL);value->stackAddress=second;break;
    case Guard:value->guardBytes=second;break;
    case Detached:if(second>1)return Error(POSIX_EINVAL);value->flags=(value->flags&~1u)|second;break;
    case Scope:if(second!=0 && second!=2)return Error(POSIX_EINVAL);value->flags=(value->flags&~2u)|second;break;
    case Policy:
        if(second<1 || second>3)return Error(POSIX_ENOTSUP);
        value->policy=second;value->priority=second==2?900:700;break;
    case Priority:{
        std::uint32_t priority{};if(!Read(memory,second,priority))return Error(POSIX_EFAULT);
        if(value->policy!=2 && (priority<256 || priority>767))return Error(POSIX_ENOTSUP);
        value->priority=priority;break;
    }
    case Inherit:if(second!=0 && second!=4)return Error(POSIX_ENOTSUP);value->inherit=second;break;
    case GetStackSize:return Write(memory,second,value->stackBytes)?0:Error(POSIX_EFAULT);
    case GetStackAddress:return Write(memory,second,value->stackAddress)?0:Error(POSIX_EFAULT);
    case GetGuard:return Write(memory,second,value->guardBytes)?0:Error(POSIX_EFAULT);
    case GetDetached:return Write(memory,second,value->flags&1)?0:Error(POSIX_EFAULT);
    case GetPriority:return Write(memory,second,value->priority)?0:Error(POSIX_EFAULT);
    case GetPolicy:return Write(memory,second,value->policy)?0:Error(POSIX_EFAULT);
    case GetInherit:return Write(memory,second,value->inherit)?0:Error(POSIX_EFAULT);
    case GetAffinity:{
        std::uint64_t mask=UINT64_MAX;
        if(value->cpuSet && !Read(memory,value->cpuSet,mask)) return Error(POSIX_EFAULT);
        return Write(memory,second,mask)?0:Error(POSIX_EFAULT);
    }
    case SetAffinity:
        value->cpuSet=handle+64;value->cpuSetBytes=16;
        if(!Write(memory,handle+64,second) || !Write(memory,handle+72,std::uint64_t{0})) return Error(POSIX_EFAULT);
        break;
    default:return Error(POSIX_EINVAL);
    }
    return Write(memory,handle,*value)?0:Error(POSIX_EFAULT);
}
}
