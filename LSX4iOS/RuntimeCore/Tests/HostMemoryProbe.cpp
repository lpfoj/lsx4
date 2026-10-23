// SPDX-License-Identifier: GPL-2.0-or-later
#include "../AppleHostServices.h"
#include <cstdio>
#include <unistd.h>
#include <dlfcn.h>

int main() {
    std::uint64_t first{}, last{};
    const auto query = [&](const void* p, int protection) {
        return Lsx4::Apple::QueryHostRange(reinterpret_cast<std::uint64_t>(p),
                                         protection, first, last);
    };
    const auto fail = [](const char* message) { std::fprintf(stderr,"%s\n",message); return 1; };
    if (!query(reinterpret_cast<void*>(&main),PROT_READ|PROT_EXEC))
        return fail("Mach-O function must be executable without a Linux address cutoff");
    if (query(reinterpret_cast<void*>(&main),PROT_WRITE)) return fail("Host code is not writable");
    // System HLE targets live in dyld shared-cache submaps, not our image.
    void* sine = dlsym(RTLD_DEFAULT,"sin");
    if (!sine || !query(sine,PROT_READ|PROT_EXEC))
        return fail("Shared-cache sin must have executable leaf permissions");
    if (query(sine,PROT_WRITE)) return fail("Shared-cache code is not writable");
    const auto page = static_cast<std::size_t>(getpagesize());
    void* p = mmap(nullptr,page,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);
    if (p==MAP_FAILED) return fail("Allocation failed");
    if (!query(p,PROT_READ|PROT_WRITE) || query(p,PROT_EXEC)) return fail("RW protection mismatch");
    if (mprotect(p,page,PROT_READ)!=0 || !query(p,PROT_READ) || query(p,PROT_WRITE))
        return fail("Protection changes must be observed");
    if (mprotect(p,page,PROT_NONE)!=0 || query(p,PROT_READ)) return fail("Guard is not readable");
    if (munmap(p,page)!=0 || query(p,PROT_READ)) return fail("Unmapped memory rejected");
    if (query(nullptr,PROT_READ) || query(reinterpret_cast<void*>(0x10),PROT_EXEC))
        return fail("Null and low invalid addresses rejected");
    std::puts("{\"status\":\"passed\",\"hostCodePermissions\":true,\"protectionTransitions\":true}");
}
