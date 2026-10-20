// SPDX-License-Identifier: MIT
#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace Lsx4::FexSemanticsProbe {

constexpr std::size_t kMxcsrCount = 4;
constexpr std::size_t kCaseCount = 26;
constexpr std::size_t kCodeCapacity = 64U * 1024U;

using Words = std::array<std::uint32_t, 4>;

enum class Operation : std::uint8_t {
  Add,
  Sub,
  Mul,
  Min,
  Max,
  Insert,
  MulAddSeparate,
  Fma231,
  FeedbackMul,
  FeedbackIir,
};

enum class Alias : std::uint8_t {
  Distinct,
  DestinationIsSource1,
  DestinationIsSource2,
};

struct CaseSpec {
  const char* name;
  Operation operation;
  Alias alias;
  std::uint8_t immediate;
  std::uint8_t repeat;
  bool denormal_sensitive;
};

inline constexpr std::array<CaseSpec, kCaseCount> kCases{{
    {"add_finite_lanes", Operation::Add, Alias::Distinct, 0, 0, false},
    {"add_alias_source1", Operation::Add,
     Alias::DestinationIsSource1, 0, 0, false},
    {"add_alias_source2", Operation::Add,
     Alias::DestinationIsSource2, 0, 0, false},
    {"sub_finite_lanes", Operation::Sub, Alias::Distinct, 0, 0, false},
    {"mul_finite_lanes", Operation::Mul, Alias::Distinct, 0, 0, false},
    {"add_denormal_input", Operation::Add, Alias::Distinct, 0, 0, true},
    {"sub_denormal_input", Operation::Sub, Alias::Distinct, 0, 0, true},
    {"mul_denormal_input", Operation::Mul, Alias::Distinct, 0, 0, true},
    {"mul_denormal_output", Operation::Mul, Alias::Distinct, 0, 0, true},
    {"feedback_decay_8", Operation::FeedbackMul, Alias::Distinct, 0, 8,
     true},
    {"feedback_iir_8", Operation::FeedbackIir, Alias::Distinct, 0, 8,
     true},
    {"add_signed_zero", Operation::Add, Alias::Distinct, 0, 0, false},
    {"min_pos_neg_zero", Operation::Min, Alias::Distinct, 0, 0, false},
    {"min_neg_pos_zero", Operation::Min, Alias::Distinct, 0, 0, false},
    {"max_pos_neg_zero", Operation::Max, Alias::Distinct, 0, 0, false},
    {"max_neg_pos_zero", Operation::Max, Alias::Distinct, 0, 0, false},
    {"min_qnan_first", Operation::Min, Alias::Distinct, 0, 0, false},
    {"min_qnan_second", Operation::Min, Alias::Distinct, 0, 0, false},
    {"max_qnan_first", Operation::Max, Alias::Distinct, 0, 0, false},
    {"max_qnan_second", Operation::Max, Alias::Distinct, 0, 0, false},
    {"insert_src3_dst1", Operation::Insert, Alias::Distinct,
     UINT8_C(0xd0), 0, false},
    {"insert_zero_mask", Operation::Insert, Alias::Distinct,
     UINT8_C(0x8a), 0, false},
    {"insert_alias_source1", Operation::Insert,
     Alias::DestinationIsSource1, UINT8_C(0x60), 0, false},
    {"insert_alias_source2", Operation::Insert,
     Alias::DestinationIsSource2, UINT8_C(0x30), 0, false},
    {"mul_add_separate", Operation::MulAddSeparate, Alias::Distinct, 0, 0,
     false},
    {"fma231_explicit", Operation::Fma231, Alias::Distinct, 0, 0, false},
}};

struct alignas(16) ProbeData {
  std::array<std::uint32_t, kMxcsrCount> mxcsr_values{};
  std::array<std::uint32_t, kMxcsrCount> mxcsr_before{};
  std::array<std::uint32_t, kMxcsrCount> mxcsr_after{};
  std::uint32_t restore_mxcsr{};
  std::array<std::uint32_t, 3> reserved{};
  std::array<Words, kCaseCount> lhs{};
  std::array<Words, kCaseCount> rhs{};
  std::array<Words, kCaseCount> addend{};
  std::array<std::array<Words, kCaseCount>, kMxcsrCount> results{};
};

static_assert(std::is_standard_layout_v<ProbeData>);
static_assert(offsetof(ProbeData, lhs) % 16 == 0);
static_assert(offsetof(ProbeData, rhs) % 16 == 0);
static_assert(offsetof(ProbeData, addend) % 16 == 0);
static_assert(offsetof(ProbeData, results) % 16 == 0);

constexpr std::uint32_t FloatBits(float value) {
  return std::bit_cast<std::uint32_t>(value);
}

inline constexpr Words kLhsUpper{
    0, UINT32_C(0x41200000), UINT32_C(0xc1a00000),
    UINT32_C(0x42500000)};
inline constexpr Words kRhsUpper{
    0, UINT32_C(0x42c80000), UINT32_C(0xc3480000),
    UINT32_C(0x43960000)};
inline constexpr Words kAddendUpper{
    0, UINT32_C(0x447a0000), UINT32_C(0xc4fa0000),
    UINT32_C(0x453b8000)};

inline Words WithLow(Words words, std::uint32_t low) {
  words[0] = low;
  return words;
}

inline void InitializeProbeData(ProbeData& data) {
  data = {};
  data.mxcsr_values = {
      UINT32_C(0x00001f80), // IEEE inputs and outputs.
      UINT32_C(0x00001fc0), // DAZ.
      UINT32_C(0x00009f80), // FTZ.
      UINT32_C(0x00009fc0), // DAZ + FTZ.
  };
  data.restore_mxcsr = UINT32_C(0x00001f80);

  for (std::size_t index = 0; index < kCaseCount; ++index) {
    data.lhs[index] = kLhsUpper;
    data.rhs[index] = kRhsUpper;
    data.addend[index] = kAddendUpper;
  }

  data.lhs[0] = WithLow(kLhsUpper, FloatBits(1.25F));
  data.rhs[0] = WithLow(kRhsUpper, FloatBits(2.5F));
  data.lhs[1] = data.lhs[0];
  data.rhs[1] = data.rhs[0];
  data.lhs[2] = data.lhs[0];
  data.rhs[2] = data.rhs[0];

  data.lhs[3] = WithLow(kLhsUpper, FloatBits(5.5F));
  data.rhs[3] = WithLow(kRhsUpper, FloatBits(2.25F));
  data.lhs[4] = WithLow(kLhsUpper, FloatBits(-3.0F));
  data.rhs[4] = WithLow(kRhsUpper, FloatBits(0.25F));

  data.lhs[5] = WithLow(kLhsUpper, UINT32_C(0x00000001));
  data.rhs[5] = WithLow(kRhsUpper, UINT32_C(0x00000000));
  data.lhs[6] = data.lhs[5];
  data.rhs[6] = data.rhs[5];
  data.lhs[7] = data.lhs[5];
  data.rhs[7] = WithLow(kRhsUpper, FloatBits(2.0F));
  data.lhs[8] = WithLow(kLhsUpper, UINT32_C(0x00800000));
  data.rhs[8] = WithLow(kRhsUpper, FloatBits(0.5F));
  data.lhs[9] = data.lhs[8];
  data.rhs[9] = data.rhs[8];
  data.lhs[10] = data.lhs[8];
  data.rhs[10] = data.rhs[8];
  data.addend[10] = WithLow(kAddendUpper, UINT32_C(0x00000001));

  data.lhs[11] = WithLow(kLhsUpper, UINT32_C(0x00000000));
  data.rhs[11] = WithLow(kRhsUpper, UINT32_C(0x80000000));
  data.lhs[12] = data.lhs[11];
  data.rhs[12] = data.rhs[11];
  data.lhs[13] = WithLow(kLhsUpper, UINT32_C(0x80000000));
  data.rhs[13] = WithLow(kRhsUpper, UINT32_C(0x00000000));
  data.lhs[14] = data.lhs[12];
  data.rhs[14] = data.rhs[12];
  data.lhs[15] = data.lhs[13];
  data.rhs[15] = data.rhs[13];

  data.lhs[16] = WithLow(kLhsUpper, UINT32_C(0x7fc12345));
  data.rhs[16] = WithLow(kRhsUpper, FloatBits(2.0F));
  data.lhs[17] = WithLow(kLhsUpper, FloatBits(2.0F));
  data.rhs[17] = WithLow(kRhsUpper, UINT32_C(0x7fc54321));
  data.lhs[18] = data.lhs[16];
  data.rhs[18] = data.rhs[16];
  data.lhs[19] = data.lhs[17];
  data.rhs[19] = data.rhs[17];

  data.lhs[20] = {UINT32_C(0x3f800000), UINT32_C(0x40000000),
                  UINT32_C(0x40400000), UINT32_C(0x40800000)};
  data.rhs[20] = {UINT32_C(0x41200000), UINT32_C(0x41a00000),
                  UINT32_C(0x41f00000), UINT32_C(0x42200000)};
  data.lhs[21] = data.lhs[20];
  data.rhs[21] = data.rhs[20];
  data.lhs[22] = data.lhs[20];
  data.rhs[22] = data.rhs[20];
  data.lhs[23] = data.lhs[20];
  data.rhs[23] = data.rhs[20];

  // The separate product rounds to exactly 1.0, so adding -1.0 yields +0.
  // A fused multiply-add retains the residual.  This pair is a direct test
  // that ordinary VMULSS+VADDSS is not contracted by the translator.
  for (std::size_t index = 24; index <= 25; ++index) {
    data.lhs[index] = WithLow(kLhsUpper, UINT32_C(0x3f800001));
    data.rhs[index] = WithLow(kRhsUpper, UINT32_C(0x3f7fffff));
    data.addend[index] = WithLow(kAddendUpper, UINT32_C(0xbf800000));
  }
}

class CodeWriter {
 public:
  CodeWriter(void* mapping, std::size_t capacity)
      : code_{static_cast<std::uint8_t*>(mapping)}, capacity_{capacity} {}

  bool Byte(std::uint8_t value) {
    if (cursor_ >= capacity_) {
      failed_ = true;
      return false;
    }
    code_[cursor_++] = value;
    return true;
  }

  bool U32(std::uint32_t value) { return Value(value); }
  bool U64(std::uint64_t value) { return Value(value); }

  [[nodiscard]] bool Good() const { return !failed_; }
  [[nodiscard]] std::size_t Size() const { return cursor_; }

 private:
  template <typename T>
  bool Value(T value) {
    if (cursor_ + sizeof(value) > capacity_) {
      failed_ = true;
      return false;
    }
    std::memcpy(code_ + cursor_, &value, sizeof(value));
    cursor_ += sizeof(value);
    return true;
  }

  std::uint8_t* code_{};
  std::size_t capacity_{};
  std::size_t cursor_{};
  bool failed_{};
};

inline bool EmitProbeProgram(void* mapping, std::size_t mapping_size,
                             ProbeData* data, bool native_return,
                             std::uint64_t guest_exit_syscall = 0,
                             std::uint32_t guest_exit_code = 0) {
  if (mapping == nullptr || data == nullptr || mapping_size == 0) {
    return false;
  }
  std::memset(mapping, 0xcc, mapping_size);
  CodeWriter writer{mapping, mapping_size};

  const auto emit_vmovups_load = [&writer](std::uint8_t reg,
                                            std::uint32_t displacement) {
    if (reg >= 8) {
      return false;
    }
    return writer.Byte(UINT8_C(0xc5)) && writer.Byte(UINT8_C(0xf8)) &&
           writer.Byte(UINT8_C(0x10)) &&
           writer.Byte(static_cast<std::uint8_t>(UINT8_C(0x87) |
                                                 (reg << 3U))) &&
           writer.U32(displacement);
  };
  const auto emit_vmovups_store = [&writer](std::uint8_t reg,
                                             std::uint32_t displacement) {
    if (reg >= 8) {
      return false;
    }
    return writer.Byte(UINT8_C(0xc5)) && writer.Byte(UINT8_C(0xf8)) &&
           writer.Byte(UINT8_C(0x11)) &&
           writer.Byte(static_cast<std::uint8_t>(UINT8_C(0x87) |
                                                 (reg << 3U))) &&
           writer.U32(displacement);
  };
  const auto emit_ldmxcsr = [&writer](std::uint32_t displacement) {
    return writer.Byte(UINT8_C(0x0f)) && writer.Byte(UINT8_C(0xae)) &&
           writer.Byte(UINT8_C(0x97)) && writer.U32(displacement);
  };
  const auto emit_stmxcsr = [&writer](std::uint32_t displacement) {
    return writer.Byte(UINT8_C(0x0f)) && writer.Byte(UINT8_C(0xae)) &&
           writer.Byte(UINT8_C(0x9f)) && writer.U32(displacement);
  };
  const auto emit_scalar = [&writer](std::uint8_t opcode,
                                      std::uint8_t destination,
                                      std::uint8_t source1,
                                      std::uint8_t source2) {
    if (destination >= 8 || source1 >= 8 || source2 >= 8) {
      return false;
    }
    const auto vex2 = static_cast<std::uint8_t>(
        UINT8_C(0x80) | ((~source1 & UINT8_C(0x0f)) << 3U) |
        UINT8_C(0x02)); // F3.0F, L=0.
    const auto modrm = static_cast<std::uint8_t>(
        UINT8_C(0xc0) | (destination << 3U) | source2);
    return writer.Byte(UINT8_C(0xc5)) && writer.Byte(vex2) &&
           writer.Byte(opcode) && writer.Byte(modrm);
  };
  const auto emit_insert = [&writer](std::uint8_t destination,
                                      std::uint8_t source1,
                                      std::uint8_t source2,
                                      std::uint8_t immediate) {
    if (destination >= 8 || source1 >= 8 || source2 >= 8) {
      return false;
    }
    const auto vex3 = static_cast<std::uint8_t>(
        ((~source1 & UINT8_C(0x0f)) << 3U) | UINT8_C(0x01));
    const auto modrm = static_cast<std::uint8_t>(
        UINT8_C(0xc0) | (destination << 3U) | source2);
    return writer.Byte(UINT8_C(0xc4)) && writer.Byte(UINT8_C(0xe3)) &&
           writer.Byte(vex3) && writer.Byte(UINT8_C(0x21)) &&
           writer.Byte(modrm) && writer.Byte(immediate);
  };
  const auto emit_fma231ss = [&writer](std::uint8_t destination,
                                       std::uint8_t source1,
                                       std::uint8_t source2) {
    if (destination >= 8 || source1 >= 8 || source2 >= 8) {
      return false;
    }
    const auto vex3 = static_cast<std::uint8_t>(
        ((~source1 & UINT8_C(0x0f)) << 3U) | UINT8_C(0x01));
    const auto modrm = static_cast<std::uint8_t>(
        UINT8_C(0xc0) | (destination << 3U) | source2);
    return writer.Byte(UINT8_C(0xc4)) && writer.Byte(UINT8_C(0xe2)) &&
           writer.Byte(vex3) && writer.Byte(UINT8_C(0xb9)) &&
           writer.Byte(modrm);
  };

  // MOV RDI, imm64.  Every following memory operand is [RDI + disp32].
  if (!writer.Byte(UINT8_C(0x48)) || !writer.Byte(UINT8_C(0xbf)) ||
      !writer.U64(reinterpret_cast<std::uint64_t>(data))) {
    return false;
  }

  for (std::size_t mxcsr_index = 0; mxcsr_index < kMxcsrCount;
       ++mxcsr_index) {
    const auto mxcsr_offset = static_cast<std::uint32_t>(
        offsetof(ProbeData, mxcsr_values) +
        mxcsr_index * sizeof(std::uint32_t));
    const auto before_offset = static_cast<std::uint32_t>(
        offsetof(ProbeData, mxcsr_before) +
        mxcsr_index * sizeof(std::uint32_t));
    const auto after_offset = static_cast<std::uint32_t>(
        offsetof(ProbeData, mxcsr_after) +
        mxcsr_index * sizeof(std::uint32_t));
    if (!emit_ldmxcsr(mxcsr_offset) || !emit_stmxcsr(before_offset)) {
      return false;
    }

    for (std::size_t case_index = 0; case_index < kCaseCount;
         ++case_index) {
      const CaseSpec& spec = kCases[case_index];
      const auto lhs_offset = static_cast<std::uint32_t>(
          offsetof(ProbeData, lhs) + case_index * sizeof(Words));
      const auto rhs_offset = static_cast<std::uint32_t>(
          offsetof(ProbeData, rhs) + case_index * sizeof(Words));
      const auto addend_offset = static_cast<std::uint32_t>(
          offsetof(ProbeData, addend) + case_index * sizeof(Words));
      const auto result_offset = static_cast<std::uint32_t>(
          offsetof(ProbeData, results) +
          (mxcsr_index * kCaseCount + case_index) * sizeof(Words));

      std::uint8_t destination = 0;
      switch (spec.alias) {
        case Alias::Distinct:
          break;
        case Alias::DestinationIsSource1:
          destination = 1;
          break;
        case Alias::DestinationIsSource2:
          destination = 2;
          break;
      }

      bool emitted = false;
      switch (spec.operation) {
        case Operation::Add:
        case Operation::Sub:
        case Operation::Mul:
        case Operation::Min:
        case Operation::Max: {
          std::uint8_t opcode = UINT8_C(0x58);
          if (spec.operation == Operation::Sub) opcode = UINT8_C(0x5c);
          if (spec.operation == Operation::Mul) opcode = UINT8_C(0x59);
          if (spec.operation == Operation::Min) opcode = UINT8_C(0x5d);
          if (spec.operation == Operation::Max) opcode = UINT8_C(0x5f);
          emitted = emit_vmovups_load(1, lhs_offset) &&
                    emit_vmovups_load(2, rhs_offset) &&
                    emit_scalar(opcode, destination, 1, 2);
          break;
        }
        case Operation::Insert:
          emitted = emit_vmovups_load(1, lhs_offset) &&
                    emit_vmovups_load(2, rhs_offset) &&
                    emit_insert(destination, 1, 2, spec.immediate);
          break;
        case Operation::MulAddSeparate:
          destination = 0;
          emitted = emit_vmovups_load(1, lhs_offset) &&
                    emit_vmovups_load(2, rhs_offset) &&
                    emit_vmovups_load(3, addend_offset) &&
                    emit_scalar(UINT8_C(0x59), 0, 1, 2) &&
                    emit_scalar(UINT8_C(0x58), 0, 0, 3);
          break;
        case Operation::Fma231:
          destination = 3;
          emitted = emit_vmovups_load(1, lhs_offset) &&
                    emit_vmovups_load(2, rhs_offset) &&
                    emit_vmovups_load(3, addend_offset) &&
                    emit_fma231ss(3, 1, 2);
          break;
        case Operation::FeedbackMul:
          destination = 0;
          emitted = emit_vmovups_load(0, lhs_offset) &&
                    emit_vmovups_load(2, rhs_offset);
          for (std::uint8_t iteration = 0;
               emitted && iteration < spec.repeat; ++iteration) {
            emitted = emit_scalar(UINT8_C(0x59), 0, 0, 2);
          }
          break;
        case Operation::FeedbackIir:
          destination = 0;
          emitted = emit_vmovups_load(0, lhs_offset) &&
                    emit_vmovups_load(2, rhs_offset) &&
                    emit_vmovups_load(3, addend_offset);
          for (std::uint8_t iteration = 0;
               emitted && iteration < spec.repeat; ++iteration) {
            emitted = emit_scalar(UINT8_C(0x59), 0, 0, 2) &&
                      emit_scalar(UINT8_C(0x58), 0, 0, 3);
          }
          break;
      }
      if (!emitted || !emit_vmovups_store(destination, result_offset)) {
        return false;
      }
    }
    if (!emit_stmxcsr(after_offset)) {
      return false;
    }
  }

  const auto restore_offset =
      static_cast<std::uint32_t>(offsetof(ProbeData, restore_mxcsr));
  if (!emit_ldmxcsr(restore_offset)) {
    return false;
  }

  if (native_return) {
    if (!writer.Byte(UINT8_C(0xc3))) {
      return false;
    }
  } else {
    if (!writer.Byte(UINT8_C(0xbf)) || !writer.U32(guest_exit_code) ||
        !writer.Byte(UINT8_C(0xb8)) ||
        !writer.U32(static_cast<std::uint32_t>(guest_exit_syscall)) ||
        !writer.Byte(UINT8_C(0x0f)) || !writer.Byte(UINT8_C(0x05)) ||
        !writer.Byte(UINT8_C(0xf4))) {
      return false;
    }
  }

#if defined(__GNUC__) || defined(__clang__)
  __builtin___clear_cache(static_cast<char*>(mapping),
                          static_cast<char*>(mapping) + writer.Size());
#endif
  return writer.Good();
}

inline bool IsNanWord(std::uint32_t value) {
  return (value & UINT32_C(0x7f800000)) == UINT32_C(0x7f800000) &&
         (value & UINT32_C(0x007fffff)) != 0;
}

inline std::uint32_t CanonicalWord(std::uint32_t value) {
  return IsNanWord(value) ? UINT32_C(0x7fc00000) : value;
}

inline std::uint64_t Digest(const ProbeData& data,
                            bool include_denormal_cases) {
  constexpr std::uint64_t kOffset = UINT64_C(1469598103934665603);
  constexpr std::uint64_t kPrime = UINT64_C(1099511628211);
  std::uint64_t hash = kOffset;
  for (std::size_t mxcsr_index = 0; mxcsr_index < kMxcsrCount;
       ++mxcsr_index) {
    for (std::size_t case_index = 0; case_index < kCaseCount;
         ++case_index) {
      if (!include_denormal_cases && kCases[case_index].denormal_sensitive) {
        continue;
      }
      for (std::uint32_t word : data.results[mxcsr_index][case_index]) {
        word = CanonicalWord(word);
        for (unsigned int byte = 0; byte < sizeof(word); ++byte) {
          hash ^= static_cast<std::uint8_t>(word >> (byte * 8U));
          hash *= kPrime;
        }
      }
    }
  }
  return hash;
}

inline Words ExpectedInsert(const Words& lhs, const Words& rhs,
                            std::uint8_t immediate) {
  Words result = lhs;
  const auto source_lane = static_cast<std::size_t>((immediate >> 6U) & 3U);
  const auto destination_lane =
      static_cast<std::size_t>((immediate >> 4U) & 3U);
  result[destination_lane] = rhs[source_lane];
  for (std::size_t lane = 0; lane < result.size(); ++lane) {
    if ((immediate & (UINT8_C(1) << lane)) != 0) {
      result[lane] = 0;
    }
  }
  return result;
}

} // namespace Lsx4::FexSemanticsProbe
