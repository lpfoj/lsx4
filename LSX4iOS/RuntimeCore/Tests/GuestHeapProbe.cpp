// SPDX-License-Identifier: GPL-2.0-or-later
#include "../AppleHostServices.h"
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <unistd.h>

namespace {
bool Check(bool passed, const char* contract) {
    if (!passed) std::fprintf(stderr, "[APPLE_GUEST_HEAP_TEST_FAILED] %s\n", contract);
    return passed;
}
}

#if defined(LSX4_GUEST_HEAP_AEROLIB_FIXTURE)
#include "core/aerolib/stubs.h"
namespace Core::AeroLib {
void* ExecutorAllocAligned(u64 alignment, u64 size);
u64 ExecutorLibcRealloc(u64 old, u64 size);
}
extern "C" bool ExecutorJitIsReadableGuestRange(std::uint64_t address, std::size_t size);

// Run only in a fresh fixture process, before another fixture or a game selects
// the allocator owner. Guest-libc ownership keeps the default HLE mspace absent,
// so the actual libc wrappers below must take the fallback arena path.
int LSX4RunAerolibGuestHeapProbe() {
    using namespace Core::AeroLib;
    const auto page = static_cast<std::size_t>(getpagesize());
    constexpr auto preferred = Lsx4::Apple::GuestHeapPreferredChunkBytes;
    constexpr std::size_t canarySize = 129, oldSize = 257, zeroSize = 97 * 3;
    if (!Check(ExecutorConfigureLibcAllocatorOwner(true), "fresh guest-libc owner")) return 1;
    const auto snapshot = [](u64 address, std::size_t requested) {
        ExecutorAllocationSnapshot info{};
        return DescribeExecutorAllocation(address, &info) && info.found && info.exact &&
            info.user_base == address && info.mapping_base == address &&
            info.mapping_size == SIZE_MAX && info.mspace_handle == 0 &&
            info.requested_size == requested && info.allocated_size >= requested;
    };
    const auto canary = ExecutorLibcMalloc(canarySize);
    if (!Check(canary && snapshot(canary, canarySize), "malloc uses tracked fallback arena")) return 2;
    if (!Check(canary % page == 0 && ExecutorJitIsReadableGuestRange(canary, preferred) &&
                   !ExecutorJitIsReadableGuestRange(canary, preferred + 1),
               "initial chunk registers exactly the Apple preferred capacity")) return 3;
    auto* canaryBytes = reinterpret_cast<unsigned char*>(canary);
    for (std::size_t i = 0; i < canarySize; ++i) canaryBytes[i] = static_cast<unsigned char>(i ^ 0x5a);
    const auto canaryIntact = [&] {
        if (!snapshot(canary, canarySize) || !ExecutorJitIsReadableGuestRange(canary, canarySize)) return false;
        for (std::size_t i = 0; i < canarySize; ++i)
            if (canaryBytes[i] != static_cast<unsigned char>(i ^ 0x5a)) return false;
        return true;
    };
    const auto zeroed = ExecutorLibcCalloc(97, 3);
    if (!Check(zeroed && snapshot(zeroed, zeroSize), "calloc uses tracked fallback arena")) return 4;
    for (std::size_t i = 0; i < zeroSize; ++i)
        if (!Check(reinterpret_cast<unsigned char*>(zeroed)[i] == 0, "calloc clears all requested bytes")) return 5;
    if (!Check(ExecutorLibcCalloc(2, UINT64_MAX) == 0, "calloc multiplication overflow")) return 6;
    const auto old = ExecutorLibcMalloc(oldSize);
    const auto aligned = reinterpret_cast<u64>(ExecutorAllocAligned(2 * page, 513));
    if (!Check(old && snapshot(old, oldSize) && aligned && aligned % (2 * page) == 0 &&
                   snapshot(aligned, 513), "aligned fallback allocations retain requested size")) return 7;
    for (std::size_t i = 0; i < oldSize; ++i)
        reinterpret_cast<unsigned char*>(old)[i] = static_cast<unsigned char>(i * 37 + 11);
    reinterpret_cast<unsigned char*>(aligned)[0] = 0x31;
    reinterpret_cast<unsigned char*>(aligned)[512] = 0x73;

    // This single allocation cannot fit the initial chunk. Do not touch a large
    // prefix merely to exhaust a bump allocator: first/last bytes suffice.
    const auto growthSize = preferred + page;
    const auto growth = ExecutorLibcMalloc(growthSize);
    if (!Check(growth && snapshot(growth, growthSize) &&
                   (growth >= canary + preferred || growth + growthSize <= canary) &&
                   ExecutorJitIsReadableGuestRange(growth, growthSize),
               "oversized request grows to a distinct fully readable chunk")) return 8;
    reinterpret_cast<unsigned char*>(growth)[0] = 0x29;
    reinterpret_cast<unsigned char*>(growth)[growthSize - 1] = 0x92;
    if (!Check(canaryIntact(), "old chunk survives replacement of the current chunk")) return 9;
    const auto largerSize = preferred + 3 * page;
    const auto moved = ExecutorLibcRealloc(old, largerSize);
    if (!Check(moved && moved != old && snapshot(moved, largerSize) &&
                   ExecutorJitIsReadableGuestRange(moved, largerSize),
               "realloc growth retains complete requested size")) return 10;
    for (std::size_t i = 0; i < oldSize; ++i)
        if (!Check(reinterpret_cast<unsigned char*>(moved)[i] == static_cast<unsigned char>(i * 37 + 11),
                   "realloc copies every previously requested byte")) return 11;
    ExecutorAllocationSnapshot forgotten{};
    if (!Check(!DescribeExecutorAllocation(old, &forgotten) && canaryIntact(),
               "successful realloc retires only the old allocation record")) return 12;
    reinterpret_cast<unsigned char*>(moved)[largerSize - 1] = 0xb4;
    const auto shrunk = ExecutorLibcRealloc(moved, 193);
    if (!Check(shrunk == moved && snapshot(shrunk, 193), "realloc shrink keeps the pointer")) return 13;
    ExecutorAllocationSnapshot beforeFailure{}, afterFailure{};
    DescribeExecutorAllocation(shrunk, &beforeFailure);
    if (!Check(ExecutorLibcRealloc(shrunk, UINT64_MAX) == 0 &&
                   DescribeExecutorAllocation(shrunk, &afterFailure) &&
                   afterFailure.serial == beforeFailure.serial && snapshot(shrunk, 193),
               "overflow failure preserves old realloc ownership and size")) return 14;
    for (std::size_t i = 0; i < 193; ++i)
        if (!Check(reinterpret_cast<unsigned char*>(shrunk)[i] == static_cast<unsigned char>(i * 37 + 11),
                   "failed realloc preserves old contents")) return 15;
    ExecutorLibcFree(zeroed);
    ExecutorAllocationSnapshot freed{};
    if (!Check(!DescribeExecutorAllocation(zeroed, &freed) && canaryIntact() &&
                   ExecutorJitIsReadableGuestRange(aligned, 513) &&
                   reinterpret_cast<unsigned char*>(aligned)[0] == 0x31 &&
                   reinterpret_cast<unsigned char*>(aligned)[512] == 0x73 &&
                   reinterpret_cast<unsigned char*>(growth)[0] == 0x29 &&
                   reinterpret_cast<unsigned char*>(growth)[growthSize - 1] == 0x92,
               "free leaves other live objects in old and new chunks intact")) return 16;
    ExecutorLibcFree(canary);
    ExecutorLibcFree(aligned);
    ExecutorLibcFree(growth);
    ExecutorLibcFree(shrunk);
    std::puts("{\"status\":\"passed\",\"actualAerolibFallback\":true,\"initialChunkBytes\":16777216,"
              "\"growthPreservesLiveObjects\":true,\"callocZeroed\":true,\"reallocGrowthAndShrink\":true,"
              "\"overflowFailurePreservesOldAllocation\":true}");
    return 0;
}
#else
int main() {
    const auto page = static_cast<std::size_t>(getpagesize());
    std::size_t capacity = 123, calls = 0, smallest = SIZE_MAX;
    auto limited = [&](std::size_t bytes) -> void* {
        ++calls;
        smallest = std::min(smallest, bytes);
        if (bytes > 4 * page) return MAP_FAILED;
        return mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    };
    void* p = Lsx4::Apple::MapGuestHeapChunkUsing(page + 1, 16 * page, page, capacity, limited);
    if (!Check(p != MAP_FAILED && capacity == 4 * page && calls == 3, "reservation backs off")) return 1;
    static_cast<volatile unsigned char*>(p)[capacity - 1] = 42;
    if (!Check(static_cast<volatile unsigned char*>(p)[capacity - 1] == 42 && !munmap(p, capacity),
               "complete successful reservation is writable")) return 2;
    calls = 0; smallest = SIZE_MAX;
    p = Lsx4::Apple::MapGuestHeapChunkUsing(5 * page + 1, 16 * page, page, capacity, limited);
    if (!Check(p == MAP_FAILED && !capacity && smallest == 6 * page && calls == 3,
               "failed reservation never tries less than rounded request")) return 3;
    calls = 0;
    p = Lsx4::Apple::MapGuestHeapChunkUsing(SIZE_MAX, page, page, capacity, limited);
    if (!Check(p == MAP_FAILED && !capacity && !calls, "request overflow rejected before mapping")) return 4;
    p = Lsx4::Apple::MapGuestHeapChunkUsing(3 * page + 1, page, page, capacity, limited);
    if (!Check(p != MAP_FAILED && capacity == 4 * page && !munmap(p, capacity),
               "minimum larger than preference is preserved")) return 5;
    for (const auto invalidPage : {std::size_t{0}, page + 1}) {
        calls = 0;
        p = Lsx4::Apple::MapGuestHeapChunkUsing(page, page, invalidPage, capacity, limited);
        if (!Check(p == MAP_FAILED && !capacity && !calls, "invalid page geometry rejected")) return 6;
    }
    calls = 0;
    p = Lsx4::Apple::MapGuestHeapChunkUsing(0, page, page, capacity, limited);
    if (!Check(p == MAP_FAILED && !capacity && !calls, "empty request rejected")) return 7;
    p = Lsx4::Apple::MapGuestHeapChunk(page + 1, 4 * page, capacity);
    if (!Check(p != MAP_FAILED && capacity == 4 * page && !munmap(p, capacity),
               "smaller caller preference remains effective")) return 8;
    p = Lsx4::Apple::MapGuestHeapChunk(page + 1, 256 * 1024 * 1024, capacity);
    if (!Check(p != MAP_FAILED && capacity == Lsx4::Apple::GuestHeapPreferredChunkBytes,
               "Apple bridge starts at 16 MiB despite Android caller preference")) return 9;
    static_cast<volatile unsigned char*>(p)[capacity - 1] = 0x65;
    if (!Check(static_cast<volatile unsigned char*>(p)[capacity - 1] == 0x65 && !munmap(p, capacity),
               "small preferred chunk is writable through its last byte")) return 10;
    const auto required = Lsx4::Apple::GuestHeapPreferredChunkBytes + page + 1;
    p = Lsx4::Apple::MapGuestHeapChunk(required, 256 * 1024 * 1024, capacity);
    if (!Check(p != MAP_FAILED && capacity == Lsx4::Apple::GuestHeapPreferredChunkBytes + 2 * page,
               "request larger than 16 MiB receives exact rounded size")) return 11;
    static_cast<volatile unsigned char*>(p)[required - 1] = 0x86;
    if (!Check(static_cast<volatile unsigned char*>(p)[required - 1] == 0x86 && !munmap(p, capacity),
               "oversized request is writable through its last byte")) return 12;
    p = Lsx4::Apple::MapGuestHeapChunk(SIZE_MAX, 256 * 1024 * 1024, capacity);
    if (!Check(p == MAP_FAILED && !capacity, "bridge request overflow rejected")) return 13;
    std::puts("{\"status\":\"passed\",\"reservationBackoff\":true,\"requestSizePreserved\":true,"
              "\"overflowRejected\":true,\"invalidGeometryRejected\":true,\"applePreferredBytes\":16777216}");
}
#endif
