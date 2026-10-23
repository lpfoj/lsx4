// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdlib>
#include <cstdio>
#include <new>
#include <unistd.h>
#include <filesystem>
#include <fstream>
#include <vector>
static thread_local bool failNextAllocation{};
void* operator new(std::size_t bytes) {
    if(failNextAllocation) { failNextAllocation=false;throw std::bad_alloc{}; }
    if(void* value=std::malloc(bytes?bytes:1))return value;
    throw std::bad_alloc{};
}
void* operator new[](std::size_t bytes) { return ::operator new(bytes); }
void operator delete(void* value) noexcept { std::free(value); }
void operator delete[](void* value) noexcept { std::free(value); }
void operator delete(void* value,std::size_t) noexcept { std::free(value); }
void operator delete[](void* value,std::size_t) noexcept { std::free(value); }
extern "C" bool executor_test_warm_profile_allocation_failure(const char*,void (*)());
extern "C" void executor_test_apple_native_failure_logging(void (*)());
int LSX4RunWarmProfileProbe() {
    executor_test_apple_native_failure_logging(+[]{failNextAllocation=true;});
    const bool loggingAllocationFree=failNextAllocation;
    failNextAllocation=false;
    if(!loggingAllocationFree) return 3;
    char path[]="/tmp/lsx4-warm-profile-XXXXXX";
    if(!mkdtemp(path))return 2;
    const bool passed=executor_test_warm_profile_allocation_failure(path,+[]{failNextAllocation=true;});
    failNextAllocation=false;
    std::filesystem::remove_all(path);
    std::printf("warm_profile_allocation_failure survived=%d fallbackLoggingAllocationFree=%d\n",
                passed,loggingAllocationFree);
    return passed?0:1;
}
