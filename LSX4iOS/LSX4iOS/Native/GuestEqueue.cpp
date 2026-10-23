// SPDX-License-Identifier: GPL-2.0-or-later
#include "GuestEqueue.h"
namespace Lsx4::Apple {
namespace {
constexpr std::uint32_t BadHandle=0x80020009,Fault=0x8002000e,Invalid=0x80020016,
    NoMemory=0x8002000c,NotFound=0x80020002,TimedOut=0x8002003c;
constexpr std::int16_t User=-11;
constexpr std::uint16_t Clear=0x20,OneShot=0x10;
std::span<std::uint8_t> Bytes(GuestEvent& event) {
    return {reinterpret_cast<std::uint8_t*>(&event),sizeof(event)};
}
}
bool GuestEqueues::Exists(std::uint64_t handle) const {
    std::scoped_lock lock(mutex_);return queues_.contains(handle);
}
std::uint32_t GuestEqueues::Add(std::uint64_t handle,GuestEvent value) {
    std::scoped_lock lock(mutex_);
    const auto queue=queues_.find(handle);if(queue==queues_.end())return BadHandle;
    auto& events=queue->second.events;
    for(auto& event:events)if(event.value.ident==value.ident && event.value.filter==value.filter) {
        event.value.userData=value.userData;return 0;
    }
    if(events.size()>=1024)return NoMemory;
    value.flags&=~1u;
    if(value.filter!=User)value.flags|=Clear;
    value.data=0;
    try {events.push_back({value,false});}catch(const std::bad_alloc&){return NoMemory;}
    return 0;
}
std::uint32_t GuestEqueues::Remove(std::uint64_t handle,std::uint64_t ident,std::int16_t filter) {
    std::scoped_lock lock(mutex_);
    const auto queue=queues_.find(handle);if(queue==queues_.end())return BadHandle;
    auto& events=queue->second.events;
    const auto event=std::ranges::find_if(events,[&](const Event& e){return e.value.ident==ident && e.value.filter==filter;});
    if(event==events.end())return NotFound;
    events.erase(event);return 0;
}
void GuestEqueues::Trigger(std::uint64_t ident,std::int16_t filter,std::uint64_t data) {
    std::scoped_lock lock(mutex_);
    for(auto& [handle,queue]:queues_)for(auto& event:queue.events)
        if(event.value.ident==ident && event.value.filter==filter) {
            ++event.value.fflags;event.value.data=data;event.triggered=true;
        }
}
void GuestEqueues::RemoveFilter(std::int16_t filter) {
    std::scoped_lock lock(mutex_);
    for(auto& [handle,queue]:queues_)std::erase_if(queue.events,[&](const Event& e){return e.value.filter==filter;});
}
std::optional<GuestEqueueResult> GuestEqueues::Call(std::string_view identity,
    const Translation::CpuFrame& frame,GuestAddressSpace& memory) {
    constexpr std::string_view suffix="#libkernel#libkernel";
    if(!identity.ends_with(suffix))return std::nullopt;
    const auto nid=identity.substr(0,identity.size()-suffix.size());
    const auto a=frame.integer[7],b=frame.integer[6],c=frame.integer[2],d=frame.integer[1],e=frame.integer[8];
    auto done=[](std::uint64_t v){return GuestEqueueResult{v,false};};
    std::scoped_lock lock(mutex_);
    if(nid=="D0OdFMjp46I") {
        if(!a || !b)return done(Invalid);
        if(!memory.CanWrite(a,8))return done(Fault);
        std::string name;
        for(unsigned i=0;i<=32;++i) {
            std::uint8_t value;
            if(b>UINT64_MAX-i || !memory.Read(b+i,{&value,1}))return done(Fault);
            if(!value) {
                if(queues_.size()>=256)return done(NoMemory);
                try {queues_.emplace(nextHandle_,Queue{std::move(name),{}});}catch(const std::bad_alloc&){return done(NoMemory);}
                memory.Put(a,nextHandle_++);return done(0);
            }
            name+=char(value);
        }
        return done(0x8002003f); // ENAMETOOLONG
    }
    if(nid=="vz+pg2zdopI" || nid=="mJ7aghmgvfc" || nid=="23CPPI1tyBY" || nid=="kwGyyjohI50") {
        GuestEvent value;
        if(!a || !memory.Read(a,Bytes(value)))throw std::runtime_error("Invalid guest event pointer");
        if(nid=="vz+pg2zdopI")return done(value.userData);
        if(nid=="mJ7aghmgvfc")return done(value.ident);
        if(nid=="23CPPI1tyBY")return done(std::uint32_t(std::int32_t(value.filter)));
        return done(value.data);
    }
    const bool known=nid=="jpFjmgAC5AE" || nid=="fzyMKs9kim0" || nid=="4R6-OvI2cEA" ||
        nid=="WDszmSbWuDk" || nid=="F6e0kwo4cnk" || nid=="LJDwdSNTnDg";
    if(!known)return std::nullopt;
    auto queue=queues_.find(a);if(queue==queues_.end())return done(BadHandle);
    if(nid=="jpFjmgAC5AE") {queues_.erase(queue);return done(0);}
    const auto ident=std::uint64_t(std::int64_t(std::int32_t(b)));
    if(nid=="4R6-OvI2cEA" || nid=="WDszmSbWuDk") {
        return done(Add(a,{ident,User,std::uint16_t(nid=="WDszmSbWuDk"?Clear:0)}));
    }
    if(nid=="LJDwdSNTnDg")return done(Remove(a,ident,User));
    if(nid=="F6e0kwo4cnk") {
        for(auto& event:queue->second.events)if(event.value.ident==ident && event.value.filter==User) {
            event.value.userData=c;event.triggered=true;return done(0);
        }
        return done(NotFound);
    }
    // Check every output before consuming events. Failed writes must not lose
    // an edge event, and an empty blocking wait must not return to guest code.
    if(!b || !d || !memory.CanWrite(d,4))return done(Fault);
    const auto count=std::int32_t(c);
    if(count<1) {memory.Put32(d,0);return done(Invalid);}
    if(!memory.CanWrite(b,std::uint64_t(count)*32))return done(Fault);
    std::uint32_t timeout=0;
    if(e && !memory.Read(e,{reinterpret_cast<std::uint8_t*>(&timeout),4}))return done(Fault);
    std::vector<GuestEvent> ready;
    for(const auto& event:queue->second.events)if(event.triggered) {
        ready.push_back(event.value);if(ready.size()==std::size_t(count))break;
    }
    if(ready.empty()) {
        if(!e || timeout)return GuestEqueueResult{0,true};
        memory.Put32(d,0);return done(TimedOut);
    }
    if(!memory.Write(b,{reinterpret_cast<const std::uint8_t*>(ready.data()),ready.size()*32}))return done(Fault);
    memory.Put32(d,ready.size());
    std::size_t consumed=0;
    for(auto it=queue->second.events.begin();it!=queue->second.events.end() && consumed<ready.size();) {
        if(!it->triggered){++it;continue;}
        ++consumed;
        if(it->value.flags&OneShot)it=queue->second.events.erase(it);
        else {
            if(it->value.flags&Clear){it->triggered=false;it->value.fflags=0;it->value.data=0;}
            ++it;
        }
    }
    return done(0);
}
}
