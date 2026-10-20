// SPDX-License-Identifier: MIT
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace Lsx4::InsideWwiseSimdProbe {

constexpr std::size_t kCaseCount = 14;
constexpr std::size_t kCodeCapacity = 16U * 1024U;

using Vector = std::array<std::uint32_t, 8>;

inline constexpr std::array<const char *, kCaseCount> kCaseNames{{
    "wwise_widen_shuffle_insert_f32x8",
    "wwise_packed32_high_f32x4",
    "wwise_packed32_low_f32x4",
    "wwise_sign_unpack_low_f32x4",
    "wwise_sign_unpack_high_f32x4",
    "wwise_widen_assembled_raw",
    "vinsertf128_dest_source1_alias",
    "vinsertf128_dest_source2_alias",
    "vmulps_dest_source1_alias",
    "vmulps_dest_source2_alias",
    "vpunpcklwd_dest_source1_alias",
    "vpunpckhwd_dest_source2_alias",
    "vpshufd_4e_ymm_lane_local",
    "split_ymm_shuffle_insert_lane_order",
}};

struct alignas(32) ProbeData {
  // Eight deliberately asymmetric signed PCM16 values in the low 128 bits.
  // The upper 128-bit sentinel detects accidental preservation after a VEX
  // XMM destination, which must zero the corresponding YMM upper half.
  Vector pcm16_packed{UINT32_C(0x80018000), UINT32_C(0xffffcfc7),
                      UINT32_C(0x00010000), UINT32_C(0x7fff3039),
                      UINT32_C(0xdeadbeef), UINT32_C(0x01234567),
                      UINT32_C(0x89abcdef), UINT32_C(0x76543210)};
  Vector pcm_scale{UINT32_C(0x38000000), UINT32_C(0x38000000),
                   UINT32_C(0x38000000), UINT32_C(0x38000000),
                   UINT32_C(0x38000000), UINT32_C(0x38000000),
                   UINT32_C(0x38000000), UINT32_C(0x38000000)};
  Vector lanes_a{UINT32_C(0x3f800000), UINT32_C(0xc0000000),
                 UINT32_C(0x3f000000), UINT32_C(0x40800000),
                 UINT32_C(0xc0a00000), UINT32_C(0x40c00000),
                 UINT32_C(0xc0e00000), UINT32_C(0x41000000)};
  Vector lanes_b{UINT32_C(0x40000000), UINT32_C(0x3e800000),
                 UINT32_C(0xc1000000), UINT32_C(0xbf800000),
                 UINT32_C(0xc0000000), UINT32_C(0x40800000),
                 UINT32_C(0x3e000000), UINT32_C(0x41800000)};
  std::array<Vector, kCaseCount> results{};
};

static_assert(std::is_standard_layout_v<ProbeData>);
static_assert(alignof(ProbeData) == 32);
static_assert(offsetof(ProbeData, results) % 32 == 0);

inline constexpr std::array<Vector, kCaseCount> kExpectedResults{{
    {UINT32_C(0xbf800000), UINT32_C(0xbf7ffe00), UINT32_C(0xbec0e400),
     UINT32_C(0xb8000000), UINT32_C(0x00000000), UINT32_C(0x38000000),
     UINT32_C(0x3ec0e400), UINT32_C(0x3f7ffe00)},
    {UINT32_C(0xbf7ffe00), UINT32_C(0xb8000000), UINT32_C(0x38000000),
     UINT32_C(0x3f7ffe00), 0, 0, 0, 0},
    {UINT32_C(0xbf800000), UINT32_C(0xbec0e400), UINT32_C(0x00000000),
     UINT32_C(0x3ec0e400), 0, 0, 0, 0},
    {UINT32_C(0xbf800000), UINT32_C(0xbf7ffe00), UINT32_C(0xbec0e400),
     UINT32_C(0xb8000000), 0, 0, 0, 0},
    {UINT32_C(0x00000000), UINT32_C(0x38000000), UINT32_C(0x3ec0e400),
     UINT32_C(0x3f7ffe00), 0, 0, 0, 0},
    {UINT32_C(0xffff8000), UINT32_C(0xffff8001), UINT32_C(0xffffcfc7),
     UINT32_C(0xffffffff), UINT32_C(0x00000000), UINT32_C(0x00000001),
     UINT32_C(0x00003039), UINT32_C(0x00007fff)},
    {UINT32_C(0x3f800000), UINT32_C(0xc0000000), UINT32_C(0x3f000000),
     UINT32_C(0x40800000), UINT32_C(0x40000000), UINT32_C(0x3e800000),
     UINT32_C(0xc1000000), UINT32_C(0xbf800000)},
    {UINT32_C(0x40000000), UINT32_C(0x3e800000), UINT32_C(0xc1000000),
     UINT32_C(0xbf800000), UINT32_C(0x3f800000), UINT32_C(0xc0000000),
     UINT32_C(0x3f000000), UINT32_C(0x40800000)},
    {UINT32_C(0x40000000), UINT32_C(0xbf000000), UINT32_C(0xc0800000),
     UINT32_C(0xc0800000), UINT32_C(0x41200000), UINT32_C(0x41c00000),
     UINT32_C(0xbf600000), UINT32_C(0x43000000)},
    {UINT32_C(0x40000000), UINT32_C(0xbf000000), UINT32_C(0xc0800000),
     UINT32_C(0xc0800000), UINT32_C(0x41200000), UINT32_C(0x41c00000),
     UINT32_C(0xbf600000), UINT32_C(0x43000000)},
    {UINT32_C(0x00008000), UINT32_C(0x40008001), UINT32_C(0x0000cfc7),
     UINT32_C(0x3e80ffff), 0, 0, 0, 0},
    {UINT32_C(0x00000000), UINT32_C(0x0001c100), UINT32_C(0x30390000),
     UINT32_C(0x7fffbf80), 0, 0, 0, 0},
    {UINT32_C(0x3f000000), UINT32_C(0x40800000), UINT32_C(0x3f800000),
     UINT32_C(0xc0000000), UINT32_C(0xc0e00000), UINT32_C(0x41000000),
     UINT32_C(0xc0a00000), UINT32_C(0x40c00000)},
    {UINT32_C(0x3f000000), UINT32_C(0x40800000), UINT32_C(0x3f800000),
     UINT32_C(0xc0000000), UINT32_C(0x3f800000), UINT32_C(0xc0000000),
     UINT32_C(0x3f000000), UINT32_C(0x40800000)},
}};

inline constexpr std::uint64_t kExpectedDigest = UINT64_C(0x2211fea2a6230e30);

class CodeWriter {
public:
  CodeWriter(void *mapping, std::size_t capacity)
      : code_{static_cast<std::uint8_t *>(mapping)}, capacity_{capacity} {}

  bool Byte(std::uint8_t value) {
    if (cursor_ >= capacity_) {
      failed_ = true;
      return false;
    }
    code_[cursor_++] = value;
    return true;
  }

  template <std::size_t Size>
  bool Bytes(const std::array<std::uint8_t, Size> &values) {
    for (const std::uint8_t value : values) {
      if (!Byte(value))
        return false;
    }
    return true;
  }

  bool U32(std::uint32_t value) { return Value(value); }
  bool U64(std::uint64_t value) { return Value(value); }
  [[nodiscard]] bool Good() const { return !failed_; }
  [[nodiscard]] std::size_t Size() const { return cursor_; }

private:
  template <typename T> bool Value(T value) {
    if (cursor_ + sizeof(value) > capacity_) {
      failed_ = true;
      return false;
    }
    std::memcpy(code_ + cursor_, &value, sizeof(value));
    cursor_ += sizeof(value);
    return true;
  }

  std::uint8_t *code_{};
  std::size_t capacity_{};
  std::size_t cursor_{};
  bool failed_{};
};

inline bool EmitProgram(void *mapping, std::size_t mapping_size,
                        ProbeData *data, bool native_return,
                        std::uint64_t guest_exit_syscall = 0,
                        std::uint32_t guest_exit_code = 0) {
  if (mapping == nullptr || data == nullptr || mapping_size == 0) {
    return false;
  }
  std::memset(mapping, 0xcc, mapping_size);
  CodeWriter writer{mapping, mapping_size};

  const auto offset = [](std::size_t value) {
    return static_cast<std::uint32_t>(value);
  };
  const auto input_offset = [&offset](std::size_t member) {
    return offset(member);
  };
  const auto result_offset = [&offset](std::size_t index) {
    return offset(offsetof(ProbeData, results) + index * sizeof(Vector));
  };

  // VMOVDQU xmm/ymm, [rdi+disp32] and its store form.  Three-byte VEX is
  // intentional: it keeps the emitter uniform while decoding to the same
  // instruction as the two-byte encodings in AkSoundEngine.prx.
  const auto emit_load = [&writer](std::uint8_t reg, std::uint32_t displacement,
                                   bool ymm) {
    const std::uint8_t vex3 = ymm ? UINT8_C(0x7e) : UINT8_C(0x7a);
    return reg < 8 && writer.Byte(UINT8_C(0xc4)) &&
           writer.Byte(UINT8_C(0xe1)) && writer.Byte(vex3) &&
           writer.Byte(UINT8_C(0x6f)) &&
           writer.Byte(
               static_cast<std::uint8_t>(UINT8_C(0x87) | (reg << 3U))) &&
           writer.U32(displacement);
  };
  const auto emit_store = [&writer](std::uint8_t reg,
                                    std::uint32_t displacement, bool ymm) {
    const std::uint8_t vex3 = ymm ? UINT8_C(0x7e) : UINT8_C(0x7a);
    return reg < 8 && writer.Byte(UINT8_C(0xc4)) &&
           writer.Byte(UINT8_C(0xe1)) && writer.Byte(vex3) &&
           writer.Byte(UINT8_C(0x7f)) &&
           writer.Byte(
               static_cast<std::uint8_t>(UINT8_C(0x87) | (reg << 3U))) &&
           writer.U32(displacement);
  };

  const auto pcm_offset = input_offset(offsetof(ProbeData, pcm16_packed));
  const auto scale_offset = input_offset(offsetof(ProbeData, pcm_scale));
  const auto lanes_a_offset = input_offset(offsetof(ProbeData, lanes_a));
  const auto lanes_b_offset = input_offset(offsetof(ProbeData, lanes_b));

  // Preserve RDI for the native Win64 oracle, then point it at ProbeData.
  if (!writer.Byte(UINT8_C(0x57)) || !writer.Byte(UINT8_C(0x48)) ||
      !writer.Byte(UINT8_C(0xbf)) ||
      !writer.U64(reinterpret_cast<std::uint64_t>(data))) {
    return false;
  }

  // Exact AkSoundEngine.prx chain at file offset 0x86d47.  Its critical
  // alias is VINSERTF128 ymm1,ymm5,xmm1,1: destination aliases source2.
  if (!emit_load(1, pcm_offset, false) || !emit_load(0, scale_offset, true) ||
      !writer.Bytes(
          std::array<std::uint8_t, 5>{0xc4, 0xe2, 0x79, 0x23, 0xe9}) ||
      !writer.Bytes(
          std::array<std::uint8_t, 5>{0xc5, 0xf9, 0x70, 0xc9, 0x4e}) ||
      !writer.Bytes(
          std::array<std::uint8_t, 5>{0xc4, 0xe2, 0x79, 0x23, 0xc9}) ||
      !writer.Bytes(
          std::array<std::uint8_t, 6>{0xc4, 0xe3, 0x55, 0x18, 0xc9, 0x01}) ||
      !writer.Bytes(std::array<std::uint8_t, 4>{0xc5, 0xfc, 0x5b, 0xc9}) ||
      !writer.Bytes(std::array<std::uint8_t, 4>{0xc5, 0xf4, 0x59, 0xc8}) ||
      !emit_store(1, result_offset(0), true)) {
    return false;
  }

  // Exact AkSoundEngine.prx chain at file offset 0x88984.  XMM1 receives
  // the high signed word of each dword, XMM2 the low signed word.
  if (!emit_load(1, pcm_offset, false) || !emit_load(0, scale_offset, false) ||
      !writer.Bytes(
          std::array<std::uint8_t, 5>{0xc5, 0xe9, 0x72, 0xf1, 0x10}) ||
      !writer.Bytes(
          std::array<std::uint8_t, 5>{0xc5, 0xf1, 0x72, 0xe1, 0x10}) ||
      !writer.Bytes(
          std::array<std::uint8_t, 5>{0xc5, 0xe9, 0x72, 0xe2, 0x10}) ||
      !writer.Bytes(std::array<std::uint8_t, 4>{0xc5, 0xf8, 0x5b, 0xc9}) ||
      !writer.Bytes(std::array<std::uint8_t, 4>{0xc5, 0xf8, 0x5b, 0xd2}) ||
      !writer.Bytes(std::array<std::uint8_t, 4>{0xc5, 0xf0, 0x59, 0xc8}) ||
      !writer.Bytes(std::array<std::uint8_t, 4>{0xc5, 0xe8, 0x59, 0xd0}) ||
      !emit_store(1, result_offset(1), true) ||
      !emit_store(2, result_offset(2), true)) {
    return false;
  }

  // Exact sign-mask/unpack chain at file offset 0x88c3d.
  if (!emit_load(1, pcm_offset, false) || !emit_load(0, scale_offset, false) ||
      !writer.Bytes(
          std::array<std::uint8_t, 5>{0xc5, 0xe1, 0x71, 0xe1, 0x0f}) ||
      !writer.Bytes(std::array<std::uint8_t, 4>{0xc5, 0xf1, 0x61, 0xe3}) ||
      !writer.Bytes(std::array<std::uint8_t, 4>{0xc5, 0xf1, 0x69, 0xcb}) ||
      !writer.Bytes(std::array<std::uint8_t, 4>{0xc5, 0xf8, 0x5b, 0xdc}) ||
      !writer.Bytes(std::array<std::uint8_t, 4>{0xc5, 0xf8, 0x5b, 0xc9}) ||
      !writer.Bytes(std::array<std::uint8_t, 4>{0xc5, 0xe0, 0x59, 0xd8}) ||
      !writer.Bytes(std::array<std::uint8_t, 4>{0xc5, 0xf0, 0x59, 0xc8}) ||
      !emit_store(3, result_offset(3), true) ||
      !emit_store(1, result_offset(4), true)) {
    return false;
  }

  // Same widening/half assembly stopped before float conversion.  This
  // isolates VPMOVSXWD, VPSHUFD 0x4e, and the source2 alias of VINSERTF128.
  if (!emit_load(1, pcm_offset, true) ||
      !writer.Bytes(
          std::array<std::uint8_t, 5>{0xc4, 0xe2, 0x79, 0x23, 0xe9}) ||
      !writer.Bytes(
          std::array<std::uint8_t, 5>{0xc5, 0xf9, 0x70, 0xc9, 0x4e}) ||
      !writer.Bytes(
          std::array<std::uint8_t, 5>{0xc4, 0xe2, 0x79, 0x23, 0xc9}) ||
      !writer.Bytes(
          std::array<std::uint8_t, 6>{0xc4, 0xe3, 0x55, 0x18, 0xc9, 0x01}) ||
      !emit_store(1, result_offset(5), true)) {
    return false;
  }

  // VINSERTF128 destination aliases source1.
  if (!emit_load(1, lanes_a_offset, true) ||
      !emit_load(2, lanes_b_offset, true) ||
      !writer.Bytes(
          std::array<std::uint8_t, 6>{0xc4, 0xe3, 0x75, 0x18, 0xca, 0x01}) ||
      !emit_store(1, result_offset(6), true)) {
    return false;
  }

  // VINSERTF128 destination aliases source2.
  if (!emit_load(1, lanes_a_offset, true) ||
      !emit_load(2, lanes_b_offset, true) ||
      !writer.Bytes(
          std::array<std::uint8_t, 6>{0xc4, 0xe3, 0x6d, 0x18, 0xc9, 0x01}) ||
      !emit_store(1, result_offset(7), true)) {
    return false;
  }

  // VMULPS destination aliases source1, then source2.
  if (!emit_load(1, lanes_a_offset, true) ||
      !emit_load(2, lanes_b_offset, true) ||
      !writer.Bytes(std::array<std::uint8_t, 4>{0xc5, 0xf4, 0x59, 0xca}) ||
      !emit_store(1, result_offset(8), true) ||
      !emit_load(1, lanes_a_offset, true) ||
      !emit_load(2, lanes_b_offset, true) ||
      !writer.Bytes(std::array<std::uint8_t, 4>{0xc5, 0xec, 0x59, 0xc9}) ||
      !emit_store(1, result_offset(9), true)) {
    return false;
  }

  // VPUNPCKLWD destination aliases source1.
  if (!emit_load(1, pcm_offset, true) || !emit_load(2, lanes_b_offset, true) ||
      !writer.Bytes(std::array<std::uint8_t, 4>{0xc5, 0xf1, 0x61, 0xca}) ||
      !emit_store(1, result_offset(10), true)) {
    return false;
  }

  // VPUNPCKHWD destination aliases source2.
  if (!emit_load(1, pcm_offset, true) || !emit_load(2, lanes_b_offset, true) ||
      !writer.Bytes(std::array<std::uint8_t, 4>{0xc5, 0xe9, 0x69, 0xc9}) ||
      !emit_store(1, result_offset(11), true)) {
    return false;
  }

  // VPSHUFD remains lane-local at 256 bits.
  if (!emit_load(1, lanes_a_offset, true) ||
      !writer.Bytes(
          std::array<std::uint8_t, 5>{0xc5, 0xfd, 0x70, 0xc9, 0x4e}) ||
      !emit_store(1, result_offset(12), true)) {
    return false;
  }

  // With SVE256 disabled, this catches swapped/dropped 128-bit halves in the
  // split lowering: shuffle both halves, then replace the high half with the
  // original low half.
  if (!emit_load(1, lanes_a_offset, true) ||
      !writer.Bytes(
          std::array<std::uint8_t, 5>{0xc5, 0xfd, 0x70, 0xd1, 0x4e}) ||
      !writer.Bytes(
          std::array<std::uint8_t, 6>{0xc4, 0xe3, 0x6d, 0x18, 0xd1, 0x01}) ||
      !emit_store(2, result_offset(13), true)) {
    return false;
  }

  // Avoid an AVX/SSE transition penalty when the native oracle returns.
  if (!writer.Bytes(std::array<std::uint8_t, 3>{0xc5, 0xf8, 0x77}) ||
      !writer.Byte(UINT8_C(0x5f))) {
    return false;
  }
  if (native_return) {
    if (!writer.Byte(UINT8_C(0xc3)))
      return false;
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
  __builtin___clear_cache(static_cast<char *>(mapping),
                          static_cast<char *>(mapping) + writer.Size());
#endif
  return writer.Good();
}

inline std::uint64_t Digest(const ProbeData &data) {
  constexpr std::uint64_t kOffset = UINT64_C(1469598103934665603);
  constexpr std::uint64_t kPrime = UINT64_C(1099511628211);
  std::uint64_t hash = kOffset;
  for (const Vector &result : data.results) {
    for (const std::uint32_t word : result) {
      hash ^= word;
      hash *= kPrime;
    }
  }
  return hash;
}

} // namespace Lsx4::InsideWwiseSimdProbe
