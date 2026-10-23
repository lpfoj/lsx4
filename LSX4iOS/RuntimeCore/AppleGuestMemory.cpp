// SPDX-License-Identifier: GPL-2.0-or-later
#include "AppleGuestMemory.h"
#include <algorithm>
#include <array>
#include <cerrno>
#include <bit>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <string>
#include <limits>
#include <sys/mman.h>
#include <unistd.h>
namespace Lsx4::Apple {
GuestMemoryLayout::GuestMemoryLayout(std::size_t bytes, bool needsExecutorHeap):backingBytes(bytes) {
    try {
        const char* temporary=std::getenv("TMPDIR");
        if(!temporary || !*temporary) throw std::runtime_error("Apple temporary directory is unavailable");
        std::string path=std::string(temporary)+"/lsx4-backing-XXXXXX";
        backingFd=mkstemp(path.data());
        if(backingFd<0) throw std::runtime_error("Apple backing file creation failed");
        unlink(path.c_str());
        if(fcntl(backingFd,F_SETFD,FD_CLOEXEC)!=0 || ftruncate(backingFd,bytes)!=0)
            throw std::runtime_error("Apple sparse backing file setup failed");
        // Physical storage is sparse. Map only requested views, rather than
        // consuming another 5-8 GiB of the limited iOS VA before a game starts.
        // Both allocator owners can need large CPU and GPU heaps. Not using
        // the Executor heap does not imply that a guest libc fits 3 GiB.
        // Use the same host-tested primary sizes for both owners, with sparse
        // on-demand additional ranges if fragmentation leaves no suitable VMA.
        // Every mapping still preserves its complete requested size and must
        // fit memory obtained from the host allocator.
        const std::array<std::size_t,7> candidates = {
            8ULL<<30,6ULL<<30,4672ULL<<20,4608ULL<<20,4ULL<<30,3ULL<<30,2ULL<<30};
        for(const auto candidate : candidates) {
            if(!candidate) break;
            void* mapped=mmap(nullptr,managedBytes+reservedBytes+candidate,PROT_NONE,
                        MAP_PRIVATE|MAP_ANON,-1,0);
            if(mapped!=MAP_FAILED) {
                guest=static_cast<std::uint8_t*>(mapped);userBytes=candidate;break;
            }
        }
        if(!guest) throw std::runtime_error(std::string("Apple guest arena reservation: ")+std::strerror(errno));
        std::fprintf(stderr,"[APPLE_GUEST_ARENA] allocator=%s base=%p managed=%zu reserved=%zu user=%zu backing=%zu\n",
            needsExecutorHeap?"executor-hle":"guest-libc",static_cast<void*>(guest),
            managedBytes,reservedBytes,userBytes,backingBytes);
    } catch(...) {
        if(backingFd>=0) close(backingFd);
        throw;
    }
}
GuestMemoryLayout::~GuestMemoryLayout() {
    if(guest) munmap(guest,GuestBytes());
    for(const auto& range:additionalRanges) munmap(reinterpret_cast<void*>(range.address),range.bytes);
    if(backingFd>=0) close(backingFd);
}
GuestMemoryLayout::HostRange GuestMemoryLayout::ReserveAdditionalRange(std::size_t bytes,
                                                                      std::size_t alignment,
                                                                      std::uintptr_t expectedAddress) {
    const auto page=static_cast<std::size_t>(getpagesize());
    alignment=std::max(alignment,page);
    if(!bytes || bytes%page || !std::has_single_bit(alignment) ||
       bytes>std::numeric_limits<std::size_t>::max()-(alignment-page)) return {};
    if(expectedAddress && (expectedAddress%alignment ||
       expectedAddress>std::numeric_limits<std::uintptr_t>::max()-bytes)) return {};
    const auto reserveBytes=bytes+alignment-page;
    // Only the host allocator selects new addresses. Never replace a mapping
    // outside an existing owned arena to satisfy a guest's advisory address.
    void* raw=mmap(reinterpret_cast<void*>(expectedAddress),reserveBytes,PROT_NONE,MAP_PRIVATE|MAP_ANON,-1,0);
    if(raw==MAP_FAILED) {
        std::fprintf(stderr,"[APPLE_GUEST_RESERVE_FAILED] expected=0x%llx bytes=%zu alignment=%zu errno=%d\n",
            static_cast<unsigned long long>(expectedAddress),bytes,alignment,errno);
        return {};
    }
    if(expectedAddress && reinterpret_cast<std::uintptr_t>(raw)!=expectedAddress) {
        // mmap may ignore a hint. Release only the new host-owned mapping;
        // never replace the region that prevented an adjacent reservation.
        munmap(raw,reserveBytes);
        return {};
    }
    const auto first=reinterpret_cast<std::uintptr_t>(raw);
    const auto aligned=(first+alignment-1)&~std::uintptr_t(alignment-1);
    const auto prefix=aligned-first,suffix=reserveBytes-prefix-bytes;
    if(prefix) munmap(raw,prefix);
    if(suffix) munmap(reinterpret_cast<void*>(aligned+bytes),suffix);
    try {
        std::unique_lock lock(additionalMutex);
        additionalRanges.push_back({aligned,bytes});
    } catch(...) {
        munmap(reinterpret_cast<void*>(aligned),bytes);
        throw;
    }
    std::fprintf(stderr,"[APPLE_GUEST_ARENA_EXTEND] base=0x%llx bytes=%zu alignment=%zu sparse=1\n",
                 static_cast<unsigned long long>(aligned),bytes,alignment);
    return {aligned,bytes};
}
bool GuestMemoryLayout::ContainsAdditionalRange(std::uintptr_t address,std::size_t bytes) const {
    if(!bytes || address>std::numeric_limits<std::uintptr_t>::max()-bytes) return false;
    std::shared_lock lock(additionalMutex);
    // Adjacent host reservations may be merged by the common VMA index.
    // Permit their union, but stop at the first unowned byte between ranges.
    const auto end=address+bytes;
    auto cursor=address;
    while(cursor<end) {
        auto next=cursor;
        const auto extend=[&](std::uintptr_t first,std::size_t length) {
            if(cursor>=first && cursor-first<length) next=std::max(next,first+length);
        };
        extend(reinterpret_cast<std::uintptr_t>(guest),GuestBytes());
        for(const auto& range:additionalRanges) extend(range.address,range.bytes);
        if(next==cursor) return false;
        cursor=next;
    }
    return true;
}
GuestMemoryLayout::View::~View() {
    if(address) munmap(address,bytes);
}
void GuestMemoryLayout::TrimViewsLocked(std::size_t targetBytes,bool idleOnly) {
    while(cachedViewBytes>targetBytes) {
        auto victim=views.end();
        for(auto it=views.begin();it!=views.end();++it) {
            if(idleOnly && it->second.use_count()!=1) continue;
            if(victim==views.end() || it->second->lastUse<victim->second->lastUse)
                victim=it;
        }
        if(victim==views.end()) break;
        cachedViewBytes-=victim->second->bytes;
        // Only the cache reference is dropped. Existing callers own their
        // mappings until their complete copy/write operation has retired.
        views.erase(victim);
    }
}
std::size_t GuestMemoryLayout::CachedBackingBytes() const {
    std::lock_guard lock(viewMutex);
    return cachedViewBytes;
}
BackingSpanLease GuestMemoryLayout::AcquireBackingSpan(std::uint64_t offset,std::size_t bytes) {
    if(!bytes || offset>=backingBytes || bytes>backingBytes-offset)
        throw std::out_of_range("Apple physical backing span exceeds guest memory");
    const auto page=static_cast<std::size_t>(getpagesize());
    const auto first=offset&~std::uint64_t(page-1);
    const auto within=static_cast<std::size_t>(offset-first);
    if(bytes>std::numeric_limits<std::size_t>::max()-within-(page-1))
        throw std::out_of_range("Apple physical backing span rounding overflow");
    const auto size=(within+bytes+page-1)&~(page-1);
    constexpr std::size_t CacheBudget=64ULL<<20;
    std::lock_guard lock(viewMutex);
    auto it=views.upper_bound(offset);
    if(it!=views.begin()) {
        --it;
        if(offset-it->first<it->second->bytes && bytes<=it->second->bytes-(offset-it->first)) {
            it->second->lastUse=++viewSequence;
            return {it->second->address+(offset-it->first),it->second};
        }
    }

    // Reclaim idle aliases before asking the constrained iOS VM for another
    // mapping. Active readers and staged writes remain pinned by their leases.
    if(size<=CacheBudget) TrimViewsLocked(CacheBudget-size,true);
    auto view=std::make_shared<View>();
    void* mapped=mmap(nullptr,size,PROT_READ|PROT_WRITE,MAP_SHARED,backingFd,first);
    if(mapped==MAP_FAILED && errno==ENOMEM) {
        TrimViewsLocked(0,true);
        mapped=mmap(nullptr,size,PROT_READ|PROT_WRITE,MAP_SHARED,backingFd,first);
    }
    if(mapped==MAP_FAILED) {
        const int error=errno;
        std::size_t pinnedBytes=0;
        for(const auto& [unused,cached]:views)
            if(cached.use_count()>1) pinnedBytes+=cached->bytes;
        std::fprintf(stderr,
            "[APPLE_BACKING_MAP_FAILED] offset=0x%llx requested=%zu mapped=%zu cache=%zu pinnedCache=%zu errno=%d\n",
            static_cast<unsigned long long>(offset),bytes,size,cachedViewBytes,pinnedBytes,error);
        throw std::runtime_error(std::string("Apple physical backing span mmap: ")+std::strerror(error));
    }
    view->address=static_cast<std::uint8_t*>(mapped);
    view->bytes=size;
    view->lastUse=++viewSequence;

    // A large one-off read must not become a permanent alias. Its lease owns
    // the complete requested span until the caller has finished copying it.
    if(size>CacheBudget) return {view->address+within,std::move(view)};

    // Keep indexed starts and ends strictly increasing: an older narrow view
    // must not hide a later wider span from the predecessor lookup above.
    auto old=views.lower_bound(first);
    decltype(views)::iterator entry;
    if(old!=views.end() && old->first==first) {
        cachedViewBytes-=old->second->bytes;
        old->second=view;
        entry=old;
    } else {
        entry=views.emplace_hint(old,first,view);
    }
    cachedViewBytes+=size;
    auto covered=std::next(entry);
    while(covered!=views.end() && covered->first+covered->second->bytes<=first+size) {
        cachedViewBytes-=covered->second->bytes;
        covered=views.erase(covered);
    }
    TrimViewsLocked(CacheBudget,true);
    // Concurrent leases can pin more than the cache budget. Dropping only
    // cache ownership keeps that budget bounded without unmapping a live span.
    TrimViewsLocked(CacheBudget,false);
    return {view->address+within,std::move(view)};
}
}
