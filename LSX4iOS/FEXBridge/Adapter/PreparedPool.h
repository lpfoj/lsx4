// SPDX-License-Identifier: MIT
// Finite synthetic fixture: reserve code before detaching the debugger. Freed
// spans are protected and never recycled; production code lifetime is separate.
#pragma once
#include "../AppleFexMemory.h"
#include <vector>
namespace AppleFexPreparedPool {
inline void* base{};
inline size_t capacity{}, consumed{};
// Published only on pool preparation/allocation. Diagnostic readers never lock
// the allocator or inspect its mutable span registry.
inline std::atomic<size_t> published_capacity{}, published_consumed{};
inline std::atomic<unsigned> exhausted_count{};
static_assert(std::atomic<size_t>::is_always_lock_free);
inline std::mutex allocation_mutex;
struct Span { void* address; size_t bytes; bool live; };
inline std::vector<Span> spans;

inline bool Prepare(size_t bytes) {
  if (base || bytes == 0 || bytes % getpagesize()) return false;
  void* memory = AppleFexMemory::Map(nullptr, bytes, PROT_READ | PROT_EXEC,
                                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (memory == MAP_FAILED) return false;
  base = memory;
  capacity = bytes;
  spans.reserve(256);
  published_capacity.store(bytes, std::memory_order_release);
  return true;
}

inline void* Map(void* hint, size_t bytes, int protection, int flags, int fd, off_t offset) {
  if (!(protection & PROT_EXEC)) return ::mmap(hint, bytes, protection, flags, fd, offset);
  const size_t page = getpagesize();
  if (!base || hint || fd != -1 || offset || flags != (MAP_PRIVATE | MAP_ANONYMOUS) ||
      !bytes || bytes > SIZE_MAX - page) { errno = EINVAL; return MAP_FAILED; }
  bytes = (bytes + page - 1) & ~(page - 1);
  std::lock_guard lock(allocation_mutex);
  if (bytes > capacity - consumed) {
    if (exhausted_count.fetch_add(1, std::memory_order_relaxed) == 0)
      std::fprintf(stderr, "[APPLE_FEX_POOL_EXHAUSTED] requested=%zu consumed=%zu capacity=%zu\n",
                   bytes, consumed, capacity);
    errno = ENOMEM; return MAP_FAILED;
  }
  void* address = static_cast<char*>(base) + consumed;
  std::memset(AppleFexMemory::Writable(address, bytes), 0, bytes);
  spans.push_back({address, bytes, true});
  consumed += bytes;
  published_consumed.store(consumed, std::memory_order_release);
  return address;
}

inline int Unmap(void* address, size_t bytes) {
  const uintptr_t at = reinterpret_cast<uintptr_t>(address);
  const uintptr_t first = reinterpret_cast<uintptr_t>(base);
  if (!base || at < first || at - first >= capacity) return ::munmap(address, bytes);
  const size_t page = getpagesize();
  if (!bytes || bytes > SIZE_MAX - page) { errno = EINVAL; return -1; }
  bytes = (bytes + page - 1) & ~(page - 1);
  std::lock_guard lock(allocation_mutex);
  for (auto& span : spans) {
    if (span.address == address && span.bytes == bytes && span.live) {
      if (!AppleFexMemory::Protect(address, bytes, FEXCore::Allocator::ProtectOptions::None)) return -1;
      span.live = false;
      return 0;
    }
  }
  errno = EINVAL;
  return -1;
}

inline void Install() {
  AppleFexMemory::Install();
  FEXCore::Allocator::mmap = Map;
  FEXCore::Allocator::munmap = Unmap;
}
}
