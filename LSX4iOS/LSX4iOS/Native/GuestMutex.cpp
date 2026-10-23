// SPDX-License-Identifier: GPL-2.0-or-later
#include "GuestMutex.h"
#include "core/libraries/kernel/posix_error.h"
#include <array>

namespace Lsx4::Apple {
namespace {
bool Read(const Translation::GuestMemoryPort& port,std::uint64_t address,std::uint64_t& value) {
    std::array<std::uint8_t,8> bytes{};
    if(!address || !port.read || !port.read(address,bytes,port.context)) return false;
    value=0;for(unsigned i=0;i<8;++i)value|=std::uint64_t(bytes[i])<<(8*i);return true;
}
bool Write(const Translation::GuestMemoryPort& port,std::uint64_t address,std::uint64_t value) {
    std::array<std::uint8_t,8> bytes{};for(unsigned i=0;i<8;++i) bytes[i]=value>>(8*i);
    return address && port.write && port.write(address,bytes,port.context);
}
bool Write32(const Translation::GuestMemoryPort& port,std::uint64_t address,std::uint32_t value) {
    std::array<std::uint8_t,4> bytes{};for(unsigned i=0;i<4;++i) bytes[i]=value>>(8*i);
    return port.write && port.write(address,bytes,port.context);
}
GuestMutexResult Error(unsigned code) {return {code ? 0x80020000u+code : 0,false};}
}

std::optional<GuestMutexResult> GuestMutexes::Call(std::string_view identity,std::uint64_t cell,
    std::uint64_t argument,std::uint64_t thread,const Translation::GuestMemoryPort& memory) {
    const auto separator=identity.find('#');
    if(separator==std::string_view::npos || identity.substr(separator)!="#libkernel#libkernel") return std::nullopt;
    const auto nid=identity.substr(0,separator);
    enum Operation {AttrInit,AttrType,AttrProtocol,AttrDestroy,Init,Destroy,Lock,TryLock,Unlock};
    Operation operation;
    if(nid=="F8bUHwAG284" || nid=="n2MMpvU8igI") operation=AttrInit;
    else if(nid=="iMp8QpE+XO4") operation=AttrType;
    else if(nid=="1FGvU0i9saQ") operation=AttrProtocol;
    else if(nid=="smWEktiyyG0") operation=AttrDestroy;
    else if(nid=="cmo1RIYva9o" || nid=="qH1gXoq71RY") operation=Init;
    else if(nid=="2Of0f+3mhhE") operation=Destroy;
    else if(nid=="9UK1vLZQft4") operation=Lock;
    else if(nid=="upoVrzMHFeE") operation=TryLock;
    else if(nid=="tn3VlD0hG60") operation=Unlock;
    else return std::nullopt;
    std::scoped_lock lock(lock_);
    std::uint64_t handle=0;
    if(!Read(memory,cell,handle)) return Error(POSIX_EFAULT);
    if(operation==AttrInit) {
        if(attributes_.size()>=4096 || nextHandle_>=StorageBase+StorageBytes) return Error(POSIX_ENOMEM);
        const auto token=nextHandle_;nextHandle_+=256;
        if(!Write(memory,token,1) || !Write(memory,token+8,0)) return Error(POSIX_EFAULT);
        attributes_.emplace(token,Attribute{});
        if(!Write(memory,cell,token)) {attributes_.erase(token);return Error(POSIX_EFAULT);}
        return Error(0);
    }
    if(operation==AttrType || operation==AttrProtocol || operation==AttrDestroy) {
        auto found=attributes_.find(handle);if(found==attributes_.end()) return Error(POSIX_EINVAL);
        if(operation==AttrDestroy) {
            if(!Write(memory,cell,0)) return Error(POSIX_EFAULT);
            attributes_.erase(found);
        } else if(operation==AttrType) {
            if(argument<1 || argument>4) return Error(POSIX_EINVAL);
            if(!Write32(memory,handle,argument)) return Error(POSIX_EFAULT);
            found->second.type=argument;
        } else {
            if(argument>2) return Error(POSIX_EINVAL);
            if(!Write32(memory,handle+4,argument)) return Error(POSIX_EFAULT);
            found->second.protocol=argument;
        }
        return Error(0);
    }
    if(operation==Init || ((operation==Lock || operation==TryLock) && handle<2)) {
        Attribute attr;
        if(operation==Init && argument) {
            std::uint64_t attributeHandle=0;
            if(!Read(memory,argument,attributeHandle)) return Error(POSIX_EFAULT);
            const auto found=attributes_.find(attributeHandle);
            if(found==attributes_.end()) return Error(POSIX_EINVAL);
            attr=found->second;
        } else if(operation!=Init && handle==1) attr.type=4;
        if(mutexes_.size()>=16384 || nextHandle_>=StorageBase+StorageBytes) return Error(POSIX_ENOMEM);
        const auto token=nextHandle_;nextHandle_+=256;
        std::array<std::uint8_t,64> prefix{};
        prefix[0x20]=attr.type;prefix[0x14]=attr.protocol;
        if(!memory.write || !memory.write(token,prefix,memory.context)) return Error(POSIX_EFAULT);
        mutexes_.emplace(token,Mutex{attr});
        if(!Write(memory,cell,token)) {mutexes_.erase(token);return Error(POSIX_EFAULT);}
        handle=token;
        if(operation==Init) return Error(0);
    }
    if(operation==Destroy && handle<2) return Write(memory,cell,2) ? Error(0) : Error(POSIX_EFAULT);
    auto found=mutexes_.find(handle);if(found==mutexes_.end()) return Error(POSIX_EINVAL);
    auto& mutex=found->second;
    // libc's fast path reads and updates the common kernel ABI prefix directly.
    std::uint64_t owner=0,counts=0,flags=0;
    if(!Read(memory,handle,owner) || !Read(memory,handle+8,counts) || !Read(memory,handle+0x20,flags)) return Error(POSIX_EFAULT);
    mutex.owner=owner;mutex.depth=owner ? static_cast<std::uint32_t>(counts)+1 : 0;
    mutex.attribute.type=flags&0xff;
    if(mutex.attribute.type<1 || mutex.attribute.type>4) return Error(POSIX_EINVAL);
    auto publish=[&] {
        return Write(memory,handle,mutex.owner) &&
               Write32(memory,handle+8,mutex.depth ? mutex.depth-1 : 0);
    };
    if(operation==Destroy) {
        if(mutex.owner) return Error(POSIX_EBUSY);
        if(!Write(memory,cell,2)) return Error(POSIX_EFAULT);
        mutexes_.erase(found);return Error(0);
    }
    if(!thread) return Error(POSIX_EINVAL);
    if(operation==Unlock) {
        if(mutex.owner!=thread) return Error(POSIX_EPERM);
        if(--mutex.depth==0) mutex.owner=0;
        return publish() ? Error(0) : Error(POSIX_EFAULT);
    }
    if(mutex.owner==thread && mutex.attribute.type==2) {
        if(mutex.depth==UINT32_MAX) return Error(POSIX_EAGAIN);
        ++mutex.depth;return publish() ? Error(0) : Error(POSIX_EFAULT);
    }
    if(mutex.owner) {
        if(operation==TryLock) return Error(POSIX_EBUSY);
        if(mutex.owner==thread && mutex.attribute.type==1) return Error(POSIX_EDEADLK);
        return GuestMutexResult{0,true};
    }
    mutex.owner=thread;mutex.depth=1;return publish() ? Error(0) : Error(POSIX_EFAULT);
}
}
