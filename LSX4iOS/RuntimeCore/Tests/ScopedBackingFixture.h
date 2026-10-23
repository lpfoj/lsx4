// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <unistd.h>

// The same observable ownership contract is checked against the compact bridge
// and the live common AddressSpace. The caller owns at least 96 MiB of backing.
namespace ScopedBackingFixture {
template<class Space, class Check>
void RunLeases(Space& space, std::uint64_t physical, Check check) {
    const auto page=static_cast<std::size_t>(getpagesize());
    {
        auto narrow=space.AcquireBackingSpan(physical+page,32);
        check(narrow.data && narrow.owner,"Narrow backing lease has no owned storage");
        std::weak_ptr<void> retired=narrow.owner;
        narrow.data[13]=0x9d;
        auto wide=space.AcquireBackingSpan(physical,3*page);
        check(wide.data && wide.owner && wide.data[page+13]==0x9d,
              "Wider backing lease lost existing physical contents");
        auto covered=space.AcquireBackingSpan(physical+2*page,32);
        check(covered.data==wide.data+2*page,
              "Covered backing request did not reuse the live wider view");
        wide.data[page+13]=0x62;
        check(narrow.data[13]==0x62,"Held narrow lease stopped observing wider writes");
        narrow.data[14]=0xd3;
        check(wide.data[page+14]==0xd3,"Held narrow lease writes lost physical aliasing");
        check(!retired.expired(),"Replacing cached coverage destroyed a held lease");
        narrow={};
        check(retired.expired(),"Superseded backing view remained alive after its last lease");
    }

    constexpr std::size_t chunk=9ULL<<20,cacheBudget=64ULL<<20;
    std::weak_ptr<void> firstIdle;
    for(std::size_t index=0;index<10;++index) {
        auto lease=space.AcquireBackingSpan(physical+index*chunk,chunk);
        check(lease.data && lease.owner,"Sequential backing lease has no owned storage");
        if(index==0) {
            firstIdle=lease.owner;
            lease.data[29]=0x7b;
        }
        lease={};
        check(space.CachedBackingBytes()<=cacheBudget,
              "Released backing views exceeded the bounded cache budget");
    }
    check(firstIdle.expired(),"Unused backing view was retained beyond the cache budget");
    auto reloaded=space.AcquireBackingSpan(physical,32);
    check(reloaded.data[29]==0x7b,"Cache eviction discarded shared physical contents");
    std::fprintf(stderr,"[APPLE_SCOPED_BACKING_TEST] heldOverlap=1 lastLeaseRelease=1 "
        "boundedCache=1 evictionPreservesContents=1 cachedBytes=%zu\n",
        static_cast<std::size_t>(space.CachedBackingBytes()));
}
}
