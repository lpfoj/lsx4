// SPDX-License-Identifier: MIT
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <type_traits>

namespace Lsx4::FexMemoryTsoProbe {

constexpr std::size_t kBufferBytes = 320;
constexpr std::size_t kByteCopyCount = 257;
constexpr std::size_t kQwordCount = 33;
constexpr std::size_t kOverlapCount = 223;
constexpr std::size_t kOverlapShift = 17;
constexpr std::uint64_t kStosQword = UINT64_C(0x6a17c3e95b204fd8);
constexpr std::uint8_t kStosByte = UINT8_C(0xa7);
constexpr std::uint32_t kPublicationIterations = 20'000;
constexpr std::uint32_t kSpinLimit = 20'000'000;
constexpr std::uint64_t kExpectedDeterministicDigest =
    UINT64_C(0x65d5e5b5670122f3);
constexpr std::size_t kFunctionStride = 4096;
constexpr std::size_t kFunctionCount = 7;
constexpr std::size_t kCodeCapacity = kFunctionStride * kFunctionCount;

enum class PublicationKind : std::uint32_t {
  RepMovs = 0,
  RepStos = 1,
  VectorStore = 2,
};

constexpr std::size_t DeterministicEntryOffset() { return 0; }
constexpr std::size_t ProducerEntryOffset(PublicationKind kind) {
  return (1U + static_cast<std::size_t>(kind) * 2U) * kFunctionStride;
}
constexpr std::size_t ConsumerEntryOffset(PublicationKind kind) {
  return ProducerEntryOffset(kind) + kFunctionStride;
}

struct alignas(64) DeterministicData {
  std::array<std::uint8_t, kBufferBytes> byte_source{};
  std::array<std::uint8_t, kBufferBytes> byte_copy{};
  std::array<std::uint8_t, kBufferBytes> overlap_forward{};
  std::array<std::uint8_t, kBufferBytes> overlap_backward{};
  std::array<std::uint64_t, kQwordCount> qword_source{};
  std::array<std::uint64_t, kQwordCount> qword_copy{};
  std::array<std::uint8_t, kBufferBytes> byte_set{};
  std::array<std::uint64_t, kQwordCount> qword_set{};
  std::array<std::uint8_t, 32> zero_count_copy_guard{};
  std::array<std::uint8_t, 32> zero_count_set_guard{};
};

struct alignas(64) PublicationData {
  alignas(64) std::array<std::uint64_t, 4> source{};
  alignas(64) std::array<std::uint64_t, 4> payload{};
  alignas(64) std::uint64_t published{};
  alignas(64) std::uint64_t acknowledged{};
  alignas(64) std::uint64_t failures{};
  std::uint64_t producer_timeout{};
  std::uint64_t consumer_timeout{};
};

struct alignas(64) ProbeData {
  DeterministicData deterministic{};
  PublicationData publication{};
};

static_assert(std::is_standard_layout_v<ProbeData>);
static_assert(alignof(ProbeData) == 64);
static_assert(offsetof(PublicationData, source) == 0);
static_assert(offsetof(PublicationData, payload) == 64);
static_assert(offsetof(PublicationData, published) == 128);
static_assert(offsetof(PublicationData, acknowledged) == 192);
static_assert(offsetof(PublicationData, failures) == 256);

inline std::uint8_t BytePattern(std::size_t index) {
  return static_cast<std::uint8_t>((index * 131U + 29U) & 0xffU);
}

inline std::uint64_t QwordPattern(std::size_t index) {
  return UINT64_C(0x8d12f761a4035bc9) ^
         (static_cast<std::uint64_t>(index) * UINT64_C(0x9e3779b97f4a7c15));
}

inline void InitializeDeterministicData(DeterministicData &data) {
  for (std::size_t index = 0; index < kBufferBytes; ++index) {
    data.byte_source[index] = BytePattern(index);
    data.byte_copy[index] = UINT8_C(0x5c);
    data.overlap_forward[index] = BytePattern(index + 41U);
    data.overlap_backward[index] = BytePattern(index + 83U);
    data.byte_set[index] = UINT8_C(0x3d);
  }
  for (std::size_t index = 0; index < kQwordCount; ++index) {
    data.qword_source[index] = QwordPattern(index);
    data.qword_copy[index] = UINT64_C(0xcccccccccccccccc);
    data.qword_set[index] = UINT64_C(0xdddddddddddddddd);
  }
  for (std::size_t index = 0; index < 32; ++index) {
    data.zero_count_copy_guard[index] =
        static_cast<std::uint8_t>(UINT8_C(0x80) + index);
    data.zero_count_set_guard[index] =
        static_cast<std::uint8_t>(UINT8_C(0x40) + index);
  }
}

inline void ResetPublicationData(PublicationData &data) { data = {}; }

inline bool ValidateDeterministicData(const DeterministicData &actual) {
  DeterministicData expected{};
  InitializeDeterministicData(expected);
  std::memcpy(expected.byte_copy.data(), expected.byte_source.data(),
              kByteCopyCount);
  std::memmove(expected.overlap_forward.data(),
               expected.overlap_forward.data() + kOverlapShift, kOverlapCount);
  std::memmove(expected.overlap_backward.data() + kOverlapShift,
               expected.overlap_backward.data(), kOverlapCount);
  std::memcpy(expected.qword_copy.data(), expected.qword_source.data(),
              kQwordCount * sizeof(std::uint64_t));
  std::memset(expected.byte_set.data(), kStosByte, kByteCopyCount);
  std::fill_n(expected.qword_set.begin(), kQwordCount, kStosQword);
  return actual.byte_source == expected.byte_source &&
         actual.byte_copy == expected.byte_copy &&
         actual.overlap_forward == expected.overlap_forward &&
         actual.overlap_backward == expected.overlap_backward &&
         actual.qword_source == expected.qword_source &&
         actual.qword_copy == expected.qword_copy &&
         actual.byte_set == expected.byte_set &&
         actual.qword_set == expected.qword_set &&
         actual.zero_count_copy_guard == expected.zero_count_copy_guard &&
         actual.zero_count_set_guard == expected.zero_count_set_guard;
}

inline bool ValidatePublicationData(const PublicationData &data,
                                    PublicationKind kind) {
  if (data.failures != 0 || data.producer_timeout != 0 ||
      data.consumer_timeout != 0 || data.published != kPublicationIterations ||
      data.acknowledged != kPublicationIterations) {
    return false;
  }
  const std::uint64_t final = kPublicationIterations;
  if (kind == PublicationKind::RepStos) {
    return data.payload[0] == final && data.payload[1] == final &&
           data.payload[2] == final && data.payload[3] == final;
  }
  if (kind == PublicationKind::VectorStore) {
    return data.payload[0] == final && data.payload[1] == ~final;
  }
  return data.payload[0] == final && data.payload[1] == ~final &&
         data.payload[2] == (final ^ UINT64_C(0x13579bdf)) &&
         data.payload[3] == (final + UINT64_C(0x2468ace));
}

inline std::uint64_t DigestDeterministicData(const DeterministicData &data) {
  constexpr std::uint64_t kOffset = UINT64_C(1469598103934665603);
  std::uint64_t hash = kOffset;
  const auto hash_bytes = [&hash](const auto &values) {
    constexpr std::uint64_t kLocalPrime = UINT64_C(1099511628211);
    const auto *bytes = reinterpret_cast<const std::uint8_t *>(values.data());
    for (std::size_t index = 0; index < sizeof(values); ++index) {
      hash ^= bytes[index];
      hash *= kLocalPrime;
    }
  };
  hash_bytes(data.byte_source);
  hash_bytes(data.byte_copy);
  hash_bytes(data.overlap_forward);
  hash_bytes(data.overlap_backward);
  hash_bytes(data.qword_source);
  hash_bytes(data.qword_copy);
  hash_bytes(data.byte_set);
  hash_bytes(data.qword_set);
  hash_bytes(data.zero_count_copy_guard);
  hash_bytes(data.zero_count_set_guard);
  return hash;
}

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
  [[nodiscard]] std::size_t Offset() const { return cursor_; }
  [[nodiscard]] bool Good() const { return !failed_; }

  std::size_t Rel32Placeholder() {
    const std::size_t offset = cursor_;
    U32(0);
    return offset;
  }

  bool PatchRel32(std::size_t displacement_offset, std::size_t target) {
    const std::int64_t delta =
        static_cast<std::int64_t>(target) -
        static_cast<std::int64_t>(displacement_offset + 4U);
    if (displacement_offset + 4U > capacity_ ||
        delta < std::numeric_limits<std::int32_t>::min() ||
        delta > std::numeric_limits<std::int32_t>::max()) {
      failed_ = true;
      return false;
    }
    const auto relative = static_cast<std::int32_t>(delta);
    std::memcpy(code_ + displacement_offset, &relative, sizeof(relative));
    return true;
  }

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

inline bool EmitMovRdiImm64(CodeWriter &writer, const void *address) {
  return writer.Bytes(std::array<std::uint8_t, 2>{0x48, 0xbf}) &&
         writer.U64(reinterpret_cast<std::uint64_t>(address));
}

inline bool EmitMovRsiImm64(CodeWriter &writer, const void *address) {
  return writer.Bytes(std::array<std::uint8_t, 2>{0x48, 0xbe}) &&
         writer.U64(reinterpret_cast<std::uint64_t>(address));
}

inline bool EmitMovRcxImm64(CodeWriter &writer, std::uint64_t value) {
  return writer.Bytes(std::array<std::uint8_t, 2>{0x48, 0xb9}) &&
         writer.U64(value);
}

inline bool EmitNativeOrGuestExit(CodeWriter &writer, bool native_return,
                                  std::uint64_t guest_exit_syscall,
                                  std::uint32_t guest_exit_code) {
  if (!writer.Byte(UINT8_C(0xfc)) || !writer.Byte(UINT8_C(0x5f)) ||
      !writer.Byte(UINT8_C(0x5e))) {
    return false;
  }
  if (native_return)
    return writer.Byte(UINT8_C(0xc3));
  return writer.Byte(UINT8_C(0xbf)) && writer.U32(guest_exit_code) &&
         writer.Byte(UINT8_C(0xb8)) &&
         writer.U32(static_cast<std::uint32_t>(guest_exit_syscall)) &&
         writer.Bytes(std::array<std::uint8_t, 3>{0x0f, 0x05, 0xf4});
}

inline bool EmitDeterministicProgram(void *mapping, std::size_t capacity,
                                     DeterministicData *data,
                                     bool native_return,
                                     std::uint64_t guest_exit_syscall = 0,
                                     std::uint32_t guest_exit_code = 0) {
  if (mapping == nullptr || data == nullptr || capacity == 0)
    return false;
  CodeWriter writer{mapping, capacity};
  if (!writer.Byte(UINT8_C(0x56)) || !writer.Byte(UINT8_C(0x57)) ||
      !writer.Byte(UINT8_C(0xfc))) {
    return false;
  }

  // Disjoint REP MOVSB.
  if (!EmitMovRsiImm64(writer, data->byte_source.data()) ||
      !EmitMovRdiImm64(writer, data->byte_copy.data()) ||
      !EmitMovRcxImm64(writer, kByteCopyCount) ||
      !writer.Bytes(std::array<std::uint8_t, 2>{0xf3, 0xa4})) {
    return false;
  }

  // memmove(dst < src): forward copy is the architectural safe direction.
  if (!EmitMovRsiImm64(writer, data->overlap_forward.data() + kOverlapShift) ||
      !EmitMovRdiImm64(writer, data->overlap_forward.data()) ||
      !EmitMovRcxImm64(writer, kOverlapCount) ||
      !writer.Bytes(std::array<std::uint8_t, 2>{0xf3, 0xa4})) {
    return false;
  }

  // memmove(dst > src): start at the end and use DF=1, then restore DF=0.
  if (!EmitMovRsiImm64(writer,
                       data->overlap_backward.data() + kOverlapCount - 1U) ||
      !EmitMovRdiImm64(writer, data->overlap_backward.data() + kOverlapShift +
                                   kOverlapCount - 1U) ||
      !EmitMovRcxImm64(writer, kOverlapCount) || !writer.Byte(UINT8_C(0xfd)) ||
      !writer.Bytes(std::array<std::uint8_t, 2>{0xf3, 0xa4}) ||
      !writer.Byte(UINT8_C(0xfc))) {
    return false;
  }

  // REP MOVSQ.
  if (!EmitMovRsiImm64(writer, data->qword_source.data()) ||
      !EmitMovRdiImm64(writer, data->qword_copy.data()) ||
      !EmitMovRcxImm64(writer, kQwordCount) ||
      !writer.Bytes(std::array<std::uint8_t, 3>{0xf3, 0x48, 0xa5})) {
    return false;
  }

  // REP STOSB and REP STOSQ.
  if (!EmitMovRdiImm64(writer, data->byte_set.data()) ||
      !EmitMovRcxImm64(writer, kByteCopyCount) || !writer.Byte(UINT8_C(0xb8)) ||
      !writer.U32(kStosByte) ||
      !writer.Bytes(std::array<std::uint8_t, 2>{0xf3, 0xaa}) ||
      !EmitMovRdiImm64(writer, data->qword_set.data()) ||
      !EmitMovRcxImm64(writer, kQwordCount) ||
      !writer.Bytes(std::array<std::uint8_t, 2>{0x48, 0xb8}) ||
      !writer.U64(kStosQword) ||
      !writer.Bytes(std::array<std::uint8_t, 3>{0xf3, 0x48, 0xab})) {
    return false;
  }

  // RCX=0 must not touch either guard for MOVS or STOS.
  if (!EmitMovRsiImm64(writer, data->zero_count_copy_guard.data() + 8U) ||
      !EmitMovRdiImm64(writer, data->zero_count_copy_guard.data()) ||
      !EmitMovRcxImm64(writer, 0) ||
      !writer.Bytes(std::array<std::uint8_t, 2>{0xf3, 0xa4}) ||
      !EmitMovRdiImm64(writer, data->zero_count_set_guard.data()) ||
      !EmitMovRcxImm64(writer, 0) || !writer.Byte(UINT8_C(0xb8)) ||
      !writer.U32(UINT32_C(0xff)) ||
      !writer.Bytes(std::array<std::uint8_t, 2>{0xf3, 0xaa})) {
    return false;
  }

  return EmitNativeOrGuestExit(writer, native_return, guest_exit_syscall,
                               guest_exit_code) &&
         writer.Good();
}

inline bool EmitJccRel32(CodeWriter &writer, std::uint8_t opcode,
                         std::size_t &patch_offset) {
  if (!writer.Bytes(std::array<std::uint8_t, 2>{0x0f, opcode}))
    return false;
  patch_offset = writer.Rel32Placeholder();
  return writer.Good();
}

inline bool EmitJmpRel32(CodeWriter &writer, std::size_t &patch_offset) {
  if (!writer.Byte(UINT8_C(0xe9)))
    return false;
  patch_offset = writer.Rel32Placeholder();
  return writer.Good();
}

inline bool EmitLoadRaxRdiDisp(CodeWriter &writer, std::uint32_t displacement) {
  return writer.Bytes(std::array<std::uint8_t, 3>{0x48, 0x8b, 0x87}) &&
         writer.U32(displacement);
}

inline bool EmitStoreRaxRdiDisp(CodeWriter &writer,
                                std::uint32_t displacement) {
  return writer.Bytes(std::array<std::uint8_t, 3>{0x48, 0x89, 0x87}) &&
         writer.U32(displacement);
}

inline bool EmitStoreR8RdiDisp(CodeWriter &writer, std::uint32_t displacement) {
  return writer.Bytes(std::array<std::uint8_t, 3>{0x4c, 0x89, 0x87}) &&
         writer.U32(displacement);
}

inline bool EmitStoreR9RdiDisp(CodeWriter &writer, std::uint32_t displacement) {
  return writer.Bytes(std::array<std::uint8_t, 3>{0x4c, 0x89, 0x8f}) &&
         writer.U32(displacement);
}

inline bool EmitTimeoutAndExit(CodeWriter &writer, PublicationData *data,
                               std::size_t timeout_offset, bool native_return,
                               std::uint64_t guest_exit_syscall,
                               std::uint32_t guest_exit_code) {
  return EmitMovRdiImm64(writer, data) && writer.Byte(UINT8_C(0xb8)) &&
         writer.U32(1) &&
         EmitStoreRaxRdiDisp(writer,
                             static_cast<std::uint32_t>(timeout_offset)) &&
         EmitNativeOrGuestExit(writer, native_return, guest_exit_syscall,
                               guest_exit_code);
}

inline bool EmitProducerProgram(void *mapping, std::size_t capacity,
                                PublicationData *data, PublicationKind kind,
                                bool native_return,
                                std::uint64_t guest_exit_syscall = 0,
                                std::uint32_t guest_exit_code = 0) {
  if (mapping == nullptr || data == nullptr || capacity == 0)
    return false;
  CodeWriter writer{mapping, capacity};
  if (!writer.Byte(UINT8_C(0x56)) || !writer.Byte(UINT8_C(0x57)) ||
      !writer.Byte(UINT8_C(0xfc)) ||
      !writer.Bytes(std::array<std::uint8_t, 2>{0x41, 0xb8}) ||
      !writer.U32(1) ||
      !writer.Bytes(std::array<std::uint8_t, 2>{0x41, 0xba}) ||
      !writer.U32(kPublicationIterations)) {
    return false;
  }

  const std::size_t outer = writer.Offset();
  if (!EmitMovRdiImm64(writer, data) ||
      !writer.Bytes(std::array<std::uint8_t, 3>{0x4d, 0x89, 0xc1}) ||
      !writer.Bytes(std::array<std::uint8_t, 3>{0x49, 0xff, 0xc9}) ||
      !writer.Bytes(std::array<std::uint8_t, 2>{0x41, 0xbb}) ||
      !writer.U32(kSpinLimit)) {
    return false;
  }
  const std::size_t wait = writer.Offset();
  if (!EmitLoadRaxRdiDisp(writer, static_cast<std::uint32_t>(offsetof(
                                      PublicationData, acknowledged))) ||
      !writer.Bytes(std::array<std::uint8_t, 3>{0x4c, 0x39, 0xc8})) {
    return false;
  }
  std::size_t ready_patch{};
  if (!EmitJccRel32(writer, UINT8_C(0x84), ready_patch) ||
      !writer.Bytes(std::array<std::uint8_t, 3>{0x41, 0xff, 0xcb})) {
    return false;
  }
  std::size_t retry_patch{};
  if (!EmitJccRel32(writer, UINT8_C(0x85), retry_patch))
    return false;
  if (!EmitTimeoutAndExit(writer, data,
                          offsetof(PublicationData, producer_timeout),
                          native_return, guest_exit_syscall, guest_exit_code)) {
    return false;
  }
  const std::size_t ready = writer.Offset();
  if (!writer.PatchRel32(ready_patch, ready) ||
      !writer.PatchRel32(retry_patch, wait) || !EmitMovRdiImm64(writer, data)) {
    return false;
  }

  if (kind == PublicationKind::RepMovs ||
      kind == PublicationKind::VectorStore) {
    if (!EmitStoreR8RdiDisp(writer, static_cast<std::uint32_t>(
                                        offsetof(PublicationData, source))) ||
        !writer.Bytes(std::array<std::uint8_t, 3>{0x4d, 0x89, 0xc1}) ||
        !writer.Bytes(std::array<std::uint8_t, 3>{0x49, 0xf7, 0xd1}) ||
        !EmitStoreR9RdiDisp(writer,
                            static_cast<std::uint32_t>(
                                offsetof(PublicationData, source) + 8U))) {
      return false;
    }
  }
  if (kind == PublicationKind::RepMovs) {
    if (!writer.Bytes(std::array<std::uint8_t, 3>{0x4d, 0x89, 0xc1}) ||
        !writer.Bytes(std::array<std::uint8_t, 3>{0x49, 0x81, 0xf1}) ||
        !writer.U32(UINT32_C(0x13579bdf)) ||
        !EmitStoreR9RdiDisp(writer,
                            static_cast<std::uint32_t>(
                                offsetof(PublicationData, source) + 16U)) ||
        !writer.Bytes(std::array<std::uint8_t, 3>{0x4d, 0x89, 0xc1}) ||
        !writer.Bytes(std::array<std::uint8_t, 3>{0x49, 0x81, 0xc1}) ||
        !writer.U32(UINT32_C(0x02468ace)) ||
        !EmitStoreR9RdiDisp(writer,
                            static_cast<std::uint32_t>(
                                offsetof(PublicationData, source) + 24U)) ||
        !EmitMovRsiImm64(writer, data->source.data()) ||
        !EmitMovRdiImm64(writer, data->payload.data()) ||
        !EmitMovRcxImm64(writer, 4) ||
        !writer.Bytes(std::array<std::uint8_t, 3>{0xf3, 0x48, 0xa5})) {
      return false;
    }
  } else if (kind == PublicationKind::RepStos) {
    if (!EmitMovRdiImm64(writer, data->payload.data()) ||
        !EmitMovRcxImm64(writer, 4) ||
        !writer.Bytes(std::array<std::uint8_t, 3>{0x4c, 0x89, 0xc0}) ||
        !writer.Bytes(std::array<std::uint8_t, 3>{0xf3, 0x48, 0xab})) {
      return false;
    }
  } else {
    if (!EmitMovRdiImm64(writer, data) ||
        !writer.Bytes(std::array<std::uint8_t, 3>{0xf3, 0x0f, 0x6f}) ||
        !writer.Byte(UINT8_C(0x87)) ||
        !writer.U32(
            static_cast<std::uint32_t>(offsetof(PublicationData, source))) ||
        !writer.Bytes(std::array<std::uint8_t, 3>{0xf3, 0x0f, 0x7f}) ||
        !writer.Byte(UINT8_C(0x87)) ||
        !writer.U32(
            static_cast<std::uint32_t>(offsetof(PublicationData, payload)))) {
      return false;
    }
  }

  if (!EmitMovRdiImm64(writer, data) ||
      !EmitStoreR8RdiDisp(writer, static_cast<std::uint32_t>(
                                      offsetof(PublicationData, published))) ||
      !writer.Bytes(std::array<std::uint8_t, 3>{0x49, 0xff, 0xc0}) ||
      !writer.Bytes(std::array<std::uint8_t, 3>{0x4d, 0x39, 0xd0})) {
    return false;
  }
  std::size_t loop_patch{};
  if (!EmitJccRel32(writer, UINT8_C(0x86), loop_patch) ||
      !writer.PatchRel32(loop_patch, outer) ||
      !EmitNativeOrGuestExit(writer, native_return, guest_exit_syscall,
                             guest_exit_code)) {
    return false;
  }
  return writer.Good();
}

inline bool EmitConsumerProgram(void *mapping, std::size_t capacity,
                                PublicationData *data, PublicationKind kind,
                                bool native_return,
                                std::uint64_t guest_exit_syscall = 0,
                                std::uint32_t guest_exit_code = 0) {
  if (mapping == nullptr || data == nullptr || capacity == 0)
    return false;
  CodeWriter writer{mapping, capacity};
  if (!writer.Byte(UINT8_C(0x56)) || !writer.Byte(UINT8_C(0x57)) ||
      !writer.Byte(UINT8_C(0xfc)) ||
      !writer.Bytes(std::array<std::uint8_t, 2>{0x41, 0xb8}) ||
      !writer.U32(1) ||
      !writer.Bytes(std::array<std::uint8_t, 2>{0x41, 0xba}) ||
      !writer.U32(kPublicationIterations)) {
    return false;
  }

  const std::size_t outer = writer.Offset();
  if (!EmitMovRdiImm64(writer, data) ||
      !writer.Bytes(std::array<std::uint8_t, 2>{0x41, 0xbb}) ||
      !writer.U32(kSpinLimit)) {
    return false;
  }
  const std::size_t wait = writer.Offset();
  if (!EmitLoadRaxRdiDisp(writer, static_cast<std::uint32_t>(
                                      offsetof(PublicationData, published))) ||
      !writer.Bytes(std::array<std::uint8_t, 3>{0x4c, 0x39, 0xc0})) {
    return false;
  }
  std::size_t ready_patch{};
  if (!EmitJccRel32(writer, UINT8_C(0x84), ready_patch) ||
      !writer.Bytes(std::array<std::uint8_t, 3>{0x41, 0xff, 0xcb})) {
    return false;
  }
  std::size_t retry_patch{};
  if (!EmitJccRel32(writer, UINT8_C(0x85), retry_patch))
    return false;
  if (!EmitTimeoutAndExit(writer, data,
                          offsetof(PublicationData, consumer_timeout),
                          native_return, guest_exit_syscall, guest_exit_code)) {
    return false;
  }
  const std::size_t ready = writer.Offset();
  if (!writer.PatchRel32(ready_patch, ready) ||
      !writer.PatchRel32(retry_patch, wait) || !EmitMovRdiImm64(writer, data)) {
    return false;
  }

  std::array<std::size_t, 4> mismatch_patches{};
  std::size_t mismatch_count{};
  const auto emit_compare_r8 = [&]() {
    return EmitLoadRaxRdiDisp(writer, static_cast<std::uint32_t>(offsetof(
                                          PublicationData, payload))) &&
           writer.Bytes(std::array<std::uint8_t, 3>{0x4c, 0x39, 0xc0}) &&
           EmitJccRel32(writer, UINT8_C(0x85),
                        mismatch_patches[mismatch_count++]);
  };
  if (!emit_compare_r8())
    return false;

  if (kind == PublicationKind::RepStos) {
    for (std::size_t index = 1; index < 4; ++index) {
      if (!EmitLoadRaxRdiDisp(
              writer, static_cast<std::uint32_t>(
                          offsetof(PublicationData, payload) + index * 8U)) ||
          !writer.Bytes(std::array<std::uint8_t, 3>{0x4c, 0x39, 0xc0}) ||
          !EmitJccRel32(writer, UINT8_C(0x85),
                        mismatch_patches[mismatch_count++])) {
        return false;
      }
    }
  } else {
    if (!writer.Bytes(std::array<std::uint8_t, 3>{0x4d, 0x89, 0xc1}) ||
        !writer.Bytes(std::array<std::uint8_t, 3>{0x49, 0xf7, 0xd1}) ||
        !EmitLoadRaxRdiDisp(writer,
                            static_cast<std::uint32_t>(
                                offsetof(PublicationData, payload) + 8U)) ||
        !writer.Bytes(std::array<std::uint8_t, 3>{0x4c, 0x39, 0xc8}) ||
        !EmitJccRel32(writer, UINT8_C(0x85),
                      mismatch_patches[mismatch_count++])) {
      return false;
    }
    if (kind == PublicationKind::RepMovs) {
      if (!writer.Bytes(std::array<std::uint8_t, 3>{0x4d, 0x89, 0xc1}) ||
          !writer.Bytes(std::array<std::uint8_t, 3>{0x49, 0x81, 0xf1}) ||
          !writer.U32(UINT32_C(0x13579bdf)) ||
          !EmitLoadRaxRdiDisp(writer,
                              static_cast<std::uint32_t>(
                                  offsetof(PublicationData, payload) + 16U)) ||
          !writer.Bytes(std::array<std::uint8_t, 3>{0x4c, 0x39, 0xc8}) ||
          !EmitJccRel32(writer, UINT8_C(0x85),
                        mismatch_patches[mismatch_count++]) ||
          !writer.Bytes(std::array<std::uint8_t, 3>{0x4d, 0x89, 0xc1}) ||
          !writer.Bytes(std::array<std::uint8_t, 3>{0x49, 0x81, 0xc1}) ||
          !writer.U32(UINT32_C(0x02468ace)) ||
          !EmitLoadRaxRdiDisp(writer,
                              static_cast<std::uint32_t>(
                                  offsetof(PublicationData, payload) + 24U)) ||
          !writer.Bytes(std::array<std::uint8_t, 3>{0x4c, 0x39, 0xc8}) ||
          !EmitJccRel32(writer, UINT8_C(0x85),
                        mismatch_patches[mismatch_count++])) {
        return false;
      }
    }
  }

  std::size_t success_patch{};
  if (!EmitJmpRel32(writer, success_patch)) {
    return false;
  }

  const std::size_t mismatch = writer.Offset();
  if (!writer.Bytes(std::array<std::uint8_t, 3>{0x48, 0xff, 0x87}) ||
      !writer.U32(
          static_cast<std::uint32_t>(offsetof(PublicationData, failures)))) {
    return false;
  }
  const std::size_t acknowledge = writer.Offset();
  if (!writer.PatchRel32(success_patch, acknowledge))
    return false;
  for (std::size_t index = 0; index < mismatch_count; ++index) {
    if (!writer.PatchRel32(mismatch_patches[index], mismatch))
      return false;
  }
  if (!EmitStoreR8RdiDisp(writer, static_cast<std::uint32_t>(offsetof(
                                      PublicationData, acknowledged))) ||
      !writer.Bytes(std::array<std::uint8_t, 3>{0x49, 0xff, 0xc0}) ||
      !writer.Bytes(std::array<std::uint8_t, 3>{0x4d, 0x39, 0xd0})) {
    return false;
  }
  std::size_t loop_patch{};
  if (!EmitJccRel32(writer, UINT8_C(0x86), loop_patch) ||
      !writer.PatchRel32(loop_patch, outer) ||
      !EmitNativeOrGuestExit(writer, native_return, guest_exit_syscall,
                             guest_exit_code)) {
    return false;
  }
  return writer.Good();
}

inline bool EmitAllPrograms(void *mapping, std::size_t mapping_size,
                            ProbeData *data, bool native_return,
                            std::uint64_t guest_exit_syscall = 0,
                            std::uint32_t deterministic_exit_code = 0,
                            std::uint32_t publication_exit_base = 0) {
  if (mapping == nullptr || data == nullptr || mapping_size < kCodeCapacity) {
    return false;
  }
  std::memset(mapping, 0xcc, mapping_size);
  auto *bytes = static_cast<std::uint8_t *>(mapping);
  if (!EmitDeterministicProgram(bytes + DeterministicEntryOffset(),
                                kFunctionStride, &data->deterministic,
                                native_return, guest_exit_syscall,
                                deterministic_exit_code)) {
    return false;
  }
  for (std::uint32_t value = 0; value < 3; ++value) {
    const auto kind = static_cast<PublicationKind>(value);
    if (!EmitProducerProgram(bytes + ProducerEntryOffset(kind), kFunctionStride,
                             &data->publication, kind, native_return,
                             guest_exit_syscall,
                             publication_exit_base + value * 2U) ||
        !EmitConsumerProgram(bytes + ConsumerEntryOffset(kind), kFunctionStride,
                             &data->publication, kind, native_return,
                             guest_exit_syscall,
                             publication_exit_base + value * 2U + 1U)) {
      return false;
    }
  }
#if defined(__GNUC__) || defined(__clang__)
  __builtin___clear_cache(static_cast<char *>(mapping),
                          static_cast<char *>(mapping) + kCodeCapacity);
#endif
  return true;
}

inline const char *PublicationKindName(PublicationKind kind) {
  switch (kind) {
  case PublicationKind::RepMovs:
    return "rep-movs-publication";
  case PublicationKind::RepStos:
    return "rep-stos-publication";
  case PublicationKind::VectorStore:
    return "vector-store-publication";
  }
  return "unknown";
}

} // namespace Lsx4::FexMemoryTsoProbe
