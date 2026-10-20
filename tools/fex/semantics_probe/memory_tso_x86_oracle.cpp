// SPDX-License-Identifier: MIT

#include "memory_tso_program.h"

#include <atomic>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>

#include <windows.h>

namespace {

using Lsx4::FexMemoryTsoProbe::DigestDeterministicData;
using Lsx4::FexMemoryTsoProbe::EmitAllPrograms;
using Lsx4::FexMemoryTsoProbe::InitializeDeterministicData;
using Lsx4::FexMemoryTsoProbe::kCodeCapacity;
using Lsx4::FexMemoryTsoProbe::ProbeData;
using Lsx4::FexMemoryTsoProbe::PublicationKind;
using Lsx4::FexMemoryTsoProbe::PublicationKindName;
using Lsx4::FexMemoryTsoProbe::ResetPublicationData;
using Lsx4::FexMemoryTsoProbe::ValidateDeterministicData;
using Lsx4::FexMemoryTsoProbe::ValidatePublicationData;

[[noreturn]] void Fail(const char *stage, std::uint64_t value = 0) {
  std::fprintf(stderr,
               "FEX_MEMORY_TSO_X86_ORACLE FAIL stage=%s value=%" PRIu64
               " error=%lu\n",
               stage, value, static_cast<unsigned long>(GetLastError()));
  std::fflush(stderr);
  std::exit(1);
}

void Require(bool condition, const char *stage, std::uint64_t value = 0) {
  if (!condition)
    Fail(stage, value);
}

using ProbeFn = void (*)();

void RunConcurrent(ProbeFn producer, ProbeFn consumer) {
  std::atomic<std::uint32_t> ready{};
  std::atomic<bool> go{};
  const auto worker = [&ready, &go](ProbeFn function) {
    ready.fetch_add(1, std::memory_order_release);
    while (!go.load(std::memory_order_acquire))
      std::this_thread::yield();
    function();
  };
  std::thread producer_thread{worker, producer};
  std::thread consumer_thread{worker, consumer};
  while (ready.load(std::memory_order_acquire) != 2)
    std::this_thread::yield();
  go.store(true, std::memory_order_release);
  producer_thread.join();
  consumer_thread.join();
}

} // namespace

int main() {
  void *code = VirtualAlloc(nullptr, kCodeCapacity, MEM_RESERVE | MEM_COMMIT,
                            PAGE_READWRITE);
  Require(code != nullptr, "virtual-alloc");

  ProbeData data{};
  InitializeDeterministicData(data.deterministic);
  Require(EmitAllPrograms(code, kCodeCapacity, &data, true), "emit-programs");

  DWORD old_protection = 0;
  Require(VirtualProtect(code, kCodeCapacity, PAGE_EXECUTE_READ,
                         &old_protection) != 0,
          "virtual-protect");
  Require(FlushInstructionCache(GetCurrentProcess(), code, kCodeCapacity) != 0,
          "flush-instruction-cache");

  auto *bytes = static_cast<std::uint8_t *>(code);
  reinterpret_cast<ProbeFn>(
      bytes + Lsx4::FexMemoryTsoProbe::DeterministicEntryOffset())();
  Require(ValidateDeterministicData(data.deterministic),
          "deterministic-rep-semantics");
  const std::uint64_t digest = DigestDeterministicData(data.deterministic);
  Require(digest == Lsx4::FexMemoryTsoProbe::kExpectedDeterministicDigest,
          "deterministic-digest", digest);
  std::printf("FEX_MEMORY_TSO_X86 deterministic=PASS digest=%016" PRIx64 "\n",
              digest);

  constexpr std::uint32_t kRounds = 8;
  for (std::uint32_t value = 0; value < 3; ++value) {
    const auto kind = static_cast<PublicationKind>(value);
    const auto producer = reinterpret_cast<ProbeFn>(
        bytes + Lsx4::FexMemoryTsoProbe::ProducerEntryOffset(kind));
    const auto consumer = reinterpret_cast<ProbeFn>(
        bytes + Lsx4::FexMemoryTsoProbe::ConsumerEntryOffset(kind));
    for (std::uint32_t round = 0; round < kRounds; ++round) {
      ResetPublicationData(data.publication);
      RunConcurrent(producer, consumer);
      if (!ValidatePublicationData(data.publication, kind)) {
        std::fprintf(
            stderr,
            "FEX_MEMORY_TSO_X86 mismatch kind=%s round=%u published=%" PRIu64
            " ack=%" PRIu64 " failures=%" PRIu64 " producerTimeout=%" PRIu64
            " consumerTimeout=%" PRIu64 "\n",
            PublicationKindName(kind), round, data.publication.published,
            data.publication.acknowledged, data.publication.failures,
            data.publication.producer_timeout,
            data.publication.consumer_timeout);
        Fail(PublicationKindName(kind), round);
      }
    }
    std::printf("FEX_MEMORY_TSO_X86 kind=%s rounds=%u iterations=%u PASS\n",
                PublicationKindName(kind), kRounds,
                Lsx4::FexMemoryTsoProbe::kPublicationIterations);
  }

  Require(VirtualFree(code, 0, MEM_RELEASE) != 0, "virtual-free");
  std::printf(
      "FEX_MEMORY_TSO_X86_ORACLE PASS deterministic=1 publicationKinds=3 "
      "rounds=%u iterationsPerRound=%u\n",
      kRounds, Lsx4::FexMemoryTsoProbe::kPublicationIterations);
  std::fflush(stdout);
  return 0;
}
