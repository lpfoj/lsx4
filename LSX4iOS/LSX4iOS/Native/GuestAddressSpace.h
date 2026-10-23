// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "GuestImage.h"
#include "executor/dynamic_translation/guest_memory.h"
#include "executor/dynamic_translation/machine_state.h"
#include <algorithm>
#include <array>
#include <mutex>
#include <memory>
#include <cstring>
#include <set>
#include <stdexcept>
namespace Lsx4::Apple {
class GuestAddressSpace {
    struct Mapping {
        std::uint64_t address;
        std::uint32_t flags;
        std::shared_ptr<std::vector<std::uint8_t>> storage;
        std::span<std::uint8_t> bytes;
    };
public:
    bool CanGpuRead(std::uint64_t address,std::size_t length) const {
        std::scoped_lock lock(mutex_);return Range(address,length,0x10);
    }
    bool CanGpuWrite(std::uint64_t address,std::size_t length) const {
        std::scoped_lock lock(mutex_);return Range(address,length,0x20);
    }
    bool ReadGpu(std::uint64_t address,std::span<std::uint8_t> out) const noexcept {
        std::scoped_lock lock(mutex_);
        if(!Range(address,out.size(),0x10)){lastFault=address;return false;}
        while(!out.empty()) {
            const auto* mapping=Find(address);const auto offset=address-mapping->address;
            const auto size=std::min<std::size_t>(out.size(),mapping->bytes.size()-offset);
            std::copy_n(mapping->bytes.data()+offset,size,out.data());out=out.subspan(size);address+=size;
        }
        return true;
    }
    bool WriteGpu(std::uint64_t address,std::span<const std::uint8_t> in) noexcept {
        std::scoped_lock lock(mutex_);
        if(!Range(address,in.size(),0x20)){lastFault=address;return false;}
        while(!in.empty()) {
            auto* mapping=Find(address);const auto offset=address-mapping->address;
            const auto size=std::min<std::size_t>(in.size(),mapping->bytes.size()-offset);
            std::copy_n(in.data(),size,mapping->bytes.data()+offset);in=in.subspan(size);address+=size;
        }
        return true;
    }
    bool IsMapped(std::uint64_t address,std::size_t length) const {
        std::scoped_lock lock(mutex_);return Range(address,length,0);
    }
    bool CanRead(std::uint64_t address,std::size_t length) const {
        std::scoped_lock lock(mutex_);return Range(address,length,4);
    }
    bool CanWrite(std::uint64_t address,std::size_t length) const {
        std::scoped_lock lock(mutex_);return Range(address,length,2);
    }
    bool IsFree(std::uint64_t address,std::uint64_t length) const {
        std::scoped_lock lock(mutex_);
        if(!length || address>UINT64_MAX-length) return false;
        for(const auto& s:segments_) if(address<s.address+s.bytes.size() && s.address<address+length)return false;
        return true;
    }
    std::uint64_t AllocatedBytes() const {
        std::scoped_lock lock(mutex_);std::uint64_t sum=0;for(const auto& s:segments_)sum+=s.bytes.size();return sum;
    }
    std::uint64_t ResidentBytes() const {
        std::scoped_lock lock(mutex_);std::uint64_t sum=0;
        std::set<const void*> seen;
        for(const auto& s:segments_)if(seen.insert(s.storage.get()).second)sum+=s.storage->size();
        return sum;
    }
    void Unmap(std::uint64_t address,std::uint64_t length) {
        std::scoped_lock lock(mutex_);
        if(!length || address>UINT64_MAX-length) throw std::runtime_error("Invalid guest unmap range");
        struct Cut {bool touched{};std::vector<Mapping> survivors;};
        std::vector<Cut> cuts(segments_.size());
        for(std::size_t i=0;i<segments_.size();++i) {
            const auto& s=segments_[i];
            const auto end=s.address+s.bytes.size();
            if(address>=end || s.address>=address+length)continue;
            auto& cut=cuts[i];cut.touched=true;
            if(s.address<address)cut.survivors.push_back({s.address,s.flags,s.storage,s.bytes.first(address-s.address)});
            if(end>address+length)cut.survivors.push_back({address+length,s.flags,s.storage,s.bytes.subspan(address+length-s.address)});
        }
        // All allocations precede mutation, so failed splitting keeps mappings intact.
        std::vector<Mapping> updated;updated.reserve(segments_.size()*2);
        for(std::size_t i=0;i<segments_.size();++i) {
            if(cuts[i].touched)for(auto& part:cuts[i].survivors)updated.push_back(std::move(part));
            else updated.push_back(std::move(segments_[i]));
        }
        segments_=std::move(updated);
    }
    void Map(GuestSegment segment) {
        auto storage=std::make_shared<std::vector<std::uint8_t>>(std::move(segment.bytes));
        MapShared(segment.address,segment.flags,storage,0,storage->size());
    }
    void MapShared(std::uint64_t address,std::uint32_t flags,
                   const std::shared_ptr<std::vector<std::uint8_t>>& storage,
                   std::size_t offset,std::size_t length,bool replace=false) {
        std::scoped_lock lock(mutex_);
        if(!storage || !length || offset>storage->size() || length>storage->size()-offset || address>UINT64_MAX-length)
            throw std::runtime_error("Invalid guest mapping");
        if(replace) {
            std::vector<Mapping> updated;updated.reserve(segments_.size()+3);
            for(const auto& s:segments_) {
                const auto end=s.address+s.bytes.size();
                if(address>=end || s.address>=address+length){updated.push_back(s);continue;}
                if(s.address<address)updated.push_back({s.address,s.flags,s.storage,s.bytes.first(address-s.address)});
                if(end>address+length)updated.push_back({address+length,s.flags,s.storage,s.bytes.subspan(address+length-s.address)});
            }
            updated.push_back({address,flags,storage,std::span(*storage).subspan(offset,length)});
            std::sort(updated.begin(),updated.end(),[](const auto& a,const auto& b){return a.address<b.address;});
            segments_=std::move(updated);return;
        }
        for (const auto& s : segments_)
            if (address < s.address+s.bytes.size() && s.address < address+length)
                throw std::runtime_error("Overlapping guest mapping");
        segments_.push_back({address,flags,storage,std::span(*storage).subspan(offset,length)});
        std::sort(segments_.begin(),segments_.end(),[](const auto& a,const auto& b){return a.address<b.address;});
    }
    void UnmapStorage(const std::shared_ptr<std::vector<std::uint8_t>>& storage,std::size_t offset,std::size_t length) {
        std::scoped_lock lock(mutex_);
        if(!storage || offset>storage->size() || length>storage->size()-offset)throw std::runtime_error("Invalid backing unmap");
        std::vector<std::pair<std::uint64_t,std::uint64_t>> ranges;
        for(const auto& mapping:segments_)if(mapping.storage==storage) {
            const auto begin=std::size_t(mapping.bytes.data()-storage->data());
            const auto start=std::max(begin,offset),end=std::min(begin+mapping.bytes.size(),offset+length);
            if(start<end)ranges.push_back({mapping.address+start-begin,end-start});
        }
        for(const auto& [address,size]:ranges)Unmap(address,size);
    }
    bool StorageMapped(const std::shared_ptr<std::vector<std::uint8_t>>& storage,std::size_t offset,std::size_t length) const {
        std::scoped_lock lock(mutex_);
        for(const auto& mapping:segments_)if(mapping.storage==storage) {
            const auto begin=std::size_t(mapping.bytes.data()-storage->data());
            if(offset<begin+mapping.bytes.size() && begin<offset+length)return true;
        }
        return false;
    }
    std::span<const std::uint8_t> Fetch(std::uint64_t address) const {
        const auto* mapping=Find(address);
        if(!mapping || !(mapping->flags&1))return {};
        return std::span<const std::uint8_t>(mapping->bytes).subspan(address-mapping->address);
    }
    bool Read(std::uint64_t address,std::span<std::uint8_t> out) const noexcept {
        std::scoped_lock lock(mutex_);
        // Most scalar accesses fit one mapping. Keep the lock and permission
        // check, but avoid a second lookup and the cross-mapping loop.
        if(const auto* s=Find(address);s && (s->flags&4) && out.size()<=s->bytes.size()-(address-s->address)) {
            if(!out.empty())std::memmove(out.data(),s->bytes.data()+(address-s->address),out.size());
            return true;
        }
        if (!Range(address,out.size(),4)) {lastFault=address;return false;}
        while (!out.empty()) {
            const auto* s=Find(address); const auto offset=address-s->address;
            const auto size=std::min<std::size_t>(out.size(),s->bytes.size()-offset);
            std::copy_n(s->bytes.data()+offset,size,out.data());
            out=out.subspan(size);address+=size;
        }
        return true;
    }
    bool Write(std::uint64_t address,std::span<const std::uint8_t> in) noexcept {
        std::scoped_lock lock(mutex_);
        if(auto* s=Find(address);s && (s->flags&2) && in.size()<=s->bytes.size()-(address-s->address)) {
            if(!in.empty())std::memmove(s->bytes.data()+(address-s->address),in.data(),in.size());
            return true;
        }
        // Validate the complete access before a crossing write can change data.
        if (!Range(address,in.size(),2)) {lastFault=address;return false;}
        while (!in.empty()) {
            auto* s=Find(address); const auto offset=address-s->address;
            const auto size=std::min<std::size_t>(in.size(),s->bytes.size()-offset);
            std::copy_n(in.data(),size,s->bytes.data()+offset);
            in=in.subspan(size);address+=size;
        }
        return true;
    }
    bool Copy(std::uint64_t destination,std::uint64_t source,std::uint64_t length) {
        std::scoped_lock lock(mutex_);
        if(!Range(source,length,4)){lastFault=source;return false;}
        if(!Range(destination,length,2)){lastFault=destination;return false;}
        std::array<std::uint8_t,65536> buffer{};
        const bool backwards=destination>source && destination-source<length;
        std::uint64_t remaining=length;
        while(remaining) {
            const auto count=std::min<std::uint64_t>(remaining,buffer.size());
            const auto offset=backwards?remaining-count:length-remaining;
            auto bytes=std::span(buffer).first(count);
            if(!Read(source+offset,bytes) || !Write(destination+offset,bytes))return false;
            remaining-=count;
        }
        return true;
    }
    bool Fill(std::uint64_t destination,std::uint8_t value,std::uint64_t length) {
        std::scoped_lock lock(mutex_);
        if(!Range(destination,length,2)){lastFault=destination;return false;}
        std::array<std::uint8_t,65536> buffer;buffer.fill(value);
        while(length) {
            const auto count=std::min<std::uint64_t>(length,buffer.size());
            if(!Write(destination,std::span(buffer).first(count)))return false;
            destination+=count;length-=count;
        }
        return true;
    }
    void Put(std::uint64_t address,std::uint64_t value) {
        std::array<std::uint8_t,8> bytes{};
        for (int i=0;i<8;++i) bytes[i]=value>>(8*i);
        if (!Write(address,bytes)) throw std::runtime_error("Invalid startup guest stack");
    }
    void Put32(std::uint64_t address,std::uint32_t value) {
        std::array<std::uint8_t,4> bytes{};for(unsigned i=0;i<4;++i) bytes[i]=value>>(8*i);
        if(!Write(address,bytes)) throw std::runtime_error("Invalid guest output pointer");
    }
    std::uint64_t Get(std::uint64_t address) const {
        std::array<std::uint8_t,8> bytes{};
        if(!Read(address,bytes)) throw std::runtime_error("Invalid guest pointer during HLE call");
        std::uint64_t value=0;for(unsigned i=0;i<8;++i) value|=std::uint64_t(bytes[i])<<(8*i);
        return value;
    }
    Translation::GuestMemoryPort Port() {
        return {[](std::uint64_t a,std::span<std::uint8_t> b,void* c) noexcept {
                    return static_cast<GuestAddressSpace*>(c)->Read(a,b);},
                [](std::uint64_t a,std::span<const std::uint8_t> b,void* c) noexcept {
                    return static_cast<GuestAddressSpace*>(c)->Write(a,b);},this,
                [](std::uint64_t address,std::uint32_t width,std::uint64_t expected,
                   std::uint64_t desired,std::uint64_t& observed,void* context) noexcept {
                    auto& space=*static_cast<GuestAddressSpace*>(context);
                    std::scoped_lock lock(space.mutex_);
                    if(width!=8 && width!=16 && width!=32 && width!=64) return false;
                    std::array<std::uint8_t,8> bytes{};
                    const auto length=width/8;
                    if(!space.Range(address,length,2) || !space.Read(address,std::span(bytes).first(length))) {
                        space.lastFault=address;return false;
                    }
                    observed=0;for(unsigned i=0;i<length;++i) observed|=std::uint64_t(bytes[i])<<(i*8);
                    if(observed!=(expected&Translation::WidthMask(width))) return true;
                    for(unsigned i=0;i<length;++i) bytes[i]=desired>>(i*8);
                    return space.Write(address,std::span(bytes).first(length));
                }};
    }
    mutable std::uint64_t lastFault{};
private:
    Mapping* Find(std::uint64_t address) noexcept {
        // Map, replacement and splitting keep these non-overlapping mappings
        // sorted. Resolve afresh, including after unmap/remap; no stale pointer
        // or permission cache can survive a guest mapping change.
        auto next=std::upper_bound(segments_.begin(),segments_.end(),address,
            [](std::uint64_t value,const Mapping& mapping){return value<mapping.address;});
        if(next==segments_.begin())return nullptr;
        auto& mapping=*std::prev(next);
        return address-mapping.address<mapping.bytes.size()?&mapping:nullptr;
    }
    const Mapping* Find(std::uint64_t address) const noexcept {
        return const_cast<GuestAddressSpace*>(this)->Find(address);
    }
    bool Range(std::uint64_t address,std::size_t length,std::uint32_t flag) const noexcept {
        if (address>UINT64_MAX-length) return false;
        while (length) {
            const auto* s=Find(address);
            if (!s || (flag && !(s->flags&flag))) return false;
            const auto size=std::min<std::size_t>(length,s->bytes.size()-(address-s->address));
            address+=size;length-=size;
        }
        return true;
    }
    std::vector<Mapping> segments_;
    mutable std::recursive_mutex mutex_;
};
}
