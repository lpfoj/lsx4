// SPDX-License-Identifier: MIT
#include "Interface/Core/SharedCodeBufferManager.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdint>
#include <thread>

TEST_CASE("Code buffer generation ranges classify every retained segment") {
  FEXCore::CPU::CodeBufferGenerationRanges Ranges;

  REQUIRE_FALSE(Ranges.Contains(0x1000));
  REQUIRE(Ranges.Publish(reinterpret_cast<void*>(0x1000), 0x800));
  REQUIRE(Ranges.Publish(reinterpret_cast<void*>(0x4000), 0x1000));

  REQUIRE(Ranges.Count() == 2);
  REQUIRE(Ranges.Contains(0x1000));
  REQUIRE(Ranges.Contains(0x17ff));
  REQUIRE_FALSE(Ranges.Contains(0x1800));
  REQUIRE(Ranges.Contains(0x4000));
  REQUIRE(Ranges.Contains(0x4fff));
  REQUIRE_FALSE(Ranges.Contains(0x5000));
}

TEST_CASE("Code buffer generation range publication is visible to signal readers") {
  FEXCore::CPU::CodeBufferGenerationRanges Ranges;
  std::atomic<bool> Start {};
  std::atomic<bool> Failed {};

  std::thread Reader {[&] {
    while (!Start.load(std::memory_order_acquire)) {}
    uint32_t Verified {};
    while (Verified != FEXCore::CPU::CodeBufferGenerationRanges::MAX_RANGES) {
      const uint32_t Published = Ranges.Count();
      while (Verified < Published) {
        const uintptr_t Begin = 0x10000 + static_cast<uintptr_t>(Verified) * 0x2000;
        if (!Ranges.Contains(Begin) || !Ranges.Contains(Begin + 0xfff) || Ranges.Contains(Begin + 0x1000)) {
          Failed.store(true, std::memory_order_relaxed);
        }
        ++Verified;
      }
    }
  }};

  Start.store(true, std::memory_order_release);
  for (uint32_t Index = 0; Index < FEXCore::CPU::CodeBufferGenerationRanges::MAX_RANGES; ++Index) {
    const uintptr_t Begin = 0x10000 + static_cast<uintptr_t>(Index) * 0x2000;
    REQUIRE(Ranges.Publish(reinterpret_cast<void*>(Begin), 0x1000));
  }
  Reader.join();

  REQUIRE_FALSE(Failed.load(std::memory_order_relaxed));
  REQUIRE_FALSE(Ranges.CanPublish());
  REQUIRE_FALSE(Ranges.Publish(reinterpret_cast<void*>(0x100000), 0x1000));
}

TEST_CASE("Code buffer generation ranges reject invalid extents") {
  FEXCore::CPU::CodeBufferGenerationRanges Ranges;
  REQUIRE_FALSE(Ranges.Publish(nullptr, 0x1000));
  REQUIRE_FALSE(Ranges.Publish(reinterpret_cast<void*>(0x1000), 0));
  REQUIRE_FALSE(Ranges.Publish(reinterpret_cast<void*>(UINTPTR_MAX - 1), 4));
  REQUIRE(Ranges.Count() == 0);
}
