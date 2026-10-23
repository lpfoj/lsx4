// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <pthread.h>
#include <sys/mman.h>
#include "AppleGuestHeap.h"
namespace Lsx4::Apple {
// A reservation preference, not an allocation limit. Larger guest requests
// still receive their complete page-rounded size, and live chunks stay mapped.
inline constexpr std::size_t GuestHeapPreferredChunkBytes = 16 * 1024 * 1024;
// Opt-in host diagnostic. No queue wait/readback occurs without a request.
using DiagnosticFrameCapture = void (*)(void* image) noexcept;
void RequestDiagnosticFrameCapture(DiagnosticFrameCapture capture) noexcept;
DiagnosticFrameCapture TakeDiagnosticFrameCapture() noexcept;
bool QueryHostRange(std::uint64_t address,int requiredProtection,std::uint64_t& first,std::uint64_t& last) noexcept;
bool QueryReadableHostRange(std::uint64_t address,std::uint64_t& first,std::uint64_t& last) noexcept;
void* ReserveFixedHostRange(std::uint64_t address,std::size_t bytes) noexcept;
std::uint64_t ContinuousTimeNanoseconds() noexcept;
inline std::uint64_t HostThreadId(pthread_t thread = nullptr) noexcept {
    std::uint64_t value{};
    return pthread_threadid_np(thread, &value) == 0 ? value : 0;
}
inline int Mincore(void* address, std::size_t bytes, unsigned char* result) noexcept {
    return ::mincore(address, bytes, reinterpret_cast<char*>(result));
}
}
