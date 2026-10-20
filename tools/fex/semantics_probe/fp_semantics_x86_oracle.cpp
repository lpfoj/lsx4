// SPDX-License-Identifier: MIT

#include "fp_semantics_program.h"

#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include <immintrin.h>
#include <windows.h>

namespace {

using Lsx4::FexSemanticsProbe::Digest;
using Lsx4::FexSemanticsProbe::EmitProbeProgram;
using Lsx4::FexSemanticsProbe::ExpectedInsert;
using Lsx4::FexSemanticsProbe::InitializeProbeData;
using Lsx4::FexSemanticsProbe::IsNanWord;
using Lsx4::FexSemanticsProbe::ProbeData;
using Lsx4::FexSemanticsProbe::Words;
using Lsx4::FexSemanticsProbe::kCaseCount;
using Lsx4::FexSemanticsProbe::kCases;
using Lsx4::FexSemanticsProbe::kCodeCapacity;
using Lsx4::FexSemanticsProbe::kMxcsrCount;

[[noreturn]] void Fail(const char* stage, std::size_t index = 0) {
  std::fprintf(stderr,
               "FEX_FP_SEMANTICS_X86_ORACLE FAIL stage=%s index=%zu\n",
               stage, index);
  std::fflush(stderr);
  std::exit(1);
}

void Require(bool condition, const char* stage, std::size_t index = 0) {
  if (!condition) {
    Fail(stage, index);
  }
}

void RequireWords(const Words& actual, const Words& expected,
                  const char* stage, std::size_t mxcsr_index) {
  if (actual != expected) {
    std::fprintf(stderr,
                 "FEX_FP_SEMANTICS_X86_ORACLE MISMATCH stage=%s "
                 "mxcsr=%zu actual=%08" PRIx32 ",%08" PRIx32
                 ",%08" PRIx32 ",%08" PRIx32 " expected=%08" PRIx32
                 ",%08" PRIx32 ",%08" PRIx32 ",%08" PRIx32 "\n",
                 stage, mxcsr_index, actual[0], actual[1], actual[2],
                 actual[3], expected[0], expected[1], expected[2],
                 expected[3]);
    Fail(stage, mxcsr_index);
  }
}

Words ScalarExpected(const Words& upper_source, std::uint32_t low) {
  Words expected = upper_source;
  expected[0] = low;
  return expected;
}

void ValidateStableSemantics(const ProbeData& data) {
  for (std::size_t mxcsr = 0; mxcsr < kMxcsrCount; ++mxcsr) {
    Require(data.mxcsr_before[mxcsr] == data.mxcsr_values[mxcsr],
            "mxcsr-roundtrip", mxcsr);

    RequireWords(data.results[mxcsr][0],
                 ScalarExpected(data.lhs[0], UINT32_C(0x40700000)),
                 "add-finite", mxcsr);
    RequireWords(data.results[mxcsr][1],
                 ScalarExpected(data.lhs[1], UINT32_C(0x40700000)),
                 "add-alias-source1", mxcsr);
    RequireWords(data.results[mxcsr][2],
                 ScalarExpected(data.lhs[2], UINT32_C(0x40700000)),
                 "add-alias-source2", mxcsr);
    RequireWords(data.results[mxcsr][3],
                 ScalarExpected(data.lhs[3], UINT32_C(0x40500000)),
                 "sub-finite", mxcsr);
    RequireWords(data.results[mxcsr][4],
                 ScalarExpected(data.lhs[4], UINT32_C(0xbf400000)),
                 "mul-finite", mxcsr);

    Require(data.results[mxcsr][11][0] == UINT32_C(0x00000000),
            "add-signed-zero", mxcsr);
    Require(data.results[mxcsr][12][0] == data.rhs[12][0],
            "min-pos-neg-zero", mxcsr);
    Require(data.results[mxcsr][13][0] == data.rhs[13][0],
            "min-neg-pos-zero", mxcsr);
    Require(data.results[mxcsr][14][0] == data.rhs[14][0],
            "max-pos-neg-zero", mxcsr);
    Require(data.results[mxcsr][15][0] == data.rhs[15][0],
            "max-neg-pos-zero", mxcsr);
    Require(data.results[mxcsr][16][0] == data.rhs[16][0],
            "min-qnan-first", mxcsr);
    Require(IsNanWord(data.results[mxcsr][17][0]),
            "min-qnan-second", mxcsr);
    Require(data.results[mxcsr][18][0] == data.rhs[18][0],
            "max-qnan-first", mxcsr);
    Require(IsNanWord(data.results[mxcsr][19][0]),
            "max-qnan-second", mxcsr);

    for (std::size_t index = 20; index <= 23; ++index) {
      RequireWords(data.results[mxcsr][index],
                   ExpectedInsert(data.lhs[index], data.rhs[index],
                                  kCases[index].immediate),
                   kCases[index].name, mxcsr);
    }

    RequireWords(data.results[mxcsr][24],
                 ScalarExpected(data.lhs[24], UINT32_C(0x00000000)),
                 "mul-add-separate", mxcsr);
    Require(data.results[mxcsr][25][0] != data.results[mxcsr][24][0],
            "fma-must-differ-from-separate", mxcsr);
    Require(data.results[mxcsr][25][1] == data.addend[25][1] &&
                data.results[mxcsr][25][2] == data.addend[25][2] &&
                data.results[mxcsr][25][3] == data.addend[25][3],
            "fma-upper-lanes", mxcsr);
  }
}

void PrintResults(const ProbeData& data) {
  for (std::size_t mxcsr = 0; mxcsr < kMxcsrCount; ++mxcsr) {
    for (std::size_t index = 0; index < kCaseCount; ++index) {
      const Words& words = data.results[mxcsr][index];
      std::printf(
          "FEX_FP_SEMANTICS_X86 mxcsr=0x%08" PRIx32
          " after=0x%08" PRIx32 " case=%s raw=%08" PRIx32
          ",%08" PRIx32 ",%08" PRIx32 ",%08" PRIx32 "\n",
          data.mxcsr_values[mxcsr], data.mxcsr_after[mxcsr],
          kCases[index].name, words[0], words[1], words[2], words[3]);
    }
  }
}

} // namespace

int main() {
  void* code = VirtualAlloc(nullptr, kCodeCapacity,
                            MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
  Require(code != nullptr, "virtual-alloc");

  ProbeData data{};
  InitializeProbeData(data);
  data.restore_mxcsr = _mm_getcsr();
  Require(EmitProbeProgram(code, kCodeCapacity, &data, true),
          "emit-program");

  DWORD old_protection = 0;
  Require(VirtualProtect(code, kCodeCapacity, PAGE_EXECUTE_READ,
                         &old_protection) != 0,
          "virtual-protect");
  Require(FlushInstructionCache(GetCurrentProcess(), code,
                                kCodeCapacity) != 0,
          "flush-instruction-cache");

  using ProbeFn = void (*)();
  reinterpret_cast<ProbeFn>(code)();
  ValidateStableSemantics(data);
  PrintResults(data);

  const std::uint64_t stable_digest = Digest(data, false);
  const std::uint64_t full_digest = Digest(data, true);
  Require(VirtualFree(code, 0, MEM_RELEASE) != 0, "virtual-free");
  std::printf(
      "FEX_FP_SEMANTICS_X86_ORACLE PASS cases=%zu mxcsr=%zu "
      "stableDigest=%016" PRIx64 " fullDigest=%016" PRIx64
      " separate=%08" PRIx32 " fused=%08" PRIx32 "\n",
      kCaseCount, kMxcsrCount, stable_digest, full_digest,
      data.results[0][24][0], data.results[0][25][0]);
  std::fflush(stdout);
  return 0;
}
