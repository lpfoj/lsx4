// SPDX-License-Identifier: MIT

#include "inside_wwise_simd_program.h"

#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include <windows.h>

namespace {

using Lsx4::InsideWwiseSimdProbe::Digest;
using Lsx4::InsideWwiseSimdProbe::EmitProgram;
using Lsx4::InsideWwiseSimdProbe::kCaseCount;
using Lsx4::InsideWwiseSimdProbe::kCaseNames;
using Lsx4::InsideWwiseSimdProbe::kCodeCapacity;
using Lsx4::InsideWwiseSimdProbe::kExpectedDigest;
using Lsx4::InsideWwiseSimdProbe::kExpectedResults;
using Lsx4::InsideWwiseSimdProbe::ProbeData;
using Lsx4::InsideWwiseSimdProbe::Vector;

[[noreturn]] void Fail(const char *stage) {
  std::fprintf(stderr, "INSIDE_WWISE_SIMD_X86 FAIL stage=%s error=%lu\n", stage,
               static_cast<unsigned long>(GetLastError()));
  std::fflush(stderr);
  std::exit(1);
}

void Require(bool condition, const char *stage) {
  if (!condition)
    Fail(stage);
}

void PrintVector(std::size_t index, const Vector &vector) {
  std::printf("INSIDE_WWISE_SIMD_X86 case=%zu name=%s raw="
              "%08" PRIx32 ",%08" PRIx32 ",%08" PRIx32 ",%08" PRIx32
              ",%08" PRIx32 ",%08" PRIx32 ",%08" PRIx32 ",%08" PRIx32 "\n",
              index, kCaseNames[index], vector[0], vector[1], vector[2],
              vector[3], vector[4], vector[5], vector[6], vector[7]);
}

} // namespace

int main() {
  void *code = VirtualAlloc(nullptr, kCodeCapacity, MEM_RESERVE | MEM_COMMIT,
                            PAGE_READWRITE);
  Require(code != nullptr, "virtual-alloc");

  ProbeData data{};
  Require(EmitProgram(code, kCodeCapacity, &data, true), "emit-program");

  DWORD old_protection = 0;
  Require(VirtualProtect(code, kCodeCapacity, PAGE_EXECUTE_READ,
                         &old_protection) != 0,
          "virtual-protect");
  Require(FlushInstructionCache(GetCurrentProcess(), code, kCodeCapacity) != 0,
          "flush-instruction-cache");

  using ProbeFn = void (*)();
  reinterpret_cast<ProbeFn>(code)();
  for (std::size_t index = 0; index < kCaseCount; ++index) {
    PrintVector(index, data.results[index]);
    Require(data.results[index] == kExpectedResults[index], kCaseNames[index]);
  }
  const std::uint64_t digest = Digest(data);
  Require(digest == kExpectedDigest, "expected-digest");
  Require(VirtualFree(code, 0, MEM_RELEASE) != 0, "virtual-free");
  std::printf("INSIDE_WWISE_SIMD_X86_ORACLE PASS cases=%zu digest=%016" PRIx64
              "\n",
              kCaseCount, digest);
  std::fflush(stdout);
  return 0;
}
