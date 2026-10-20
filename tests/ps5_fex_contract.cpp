// SPDX-FileCopyrightText: Copyright 2026 LSX4 Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "executor/ps5_desktop/fex_contract.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

[[noreturn]] void Fail(const char *message) {
  std::fprintf(stderr, "ps5_fex_contract: %s\n", message);
  std::exit(1);
}

void Require(const bool condition, const char *message) {
  if (!condition) {
    Fail(message);
  }
}

} // namespace

int main() {
  std::array<std::uint8_t, Executor::Ps5Fex::HleGuestStubSize> guest_stub{};
  std::array<std::uint8_t, Executor::Ps5Fex::HleThunkIdentitySize> identity{};
  for (std::size_t index = 0; index != identity.size(); ++index) {
    identity[index] = static_cast<std::uint8_t>(index ^ 0xa5u);
  }
  constexpr std::uint64_t StubThunk = UINT64_C(0x8f011c400);
  std::array<std::uint8_t, Executor::Ps5Fex::HleGuestStubSize>
      current_guest_stub{};
  const auto current_emitted = Executor::Ps5Fex::BuildCurrentHleGuestStub(
      current_guest_stub, StubThunk, identity);
  constexpr std::array<std::uint8_t, 5> CurrentThunkTail{
      0x48, 0x89, 0xe7, 0x0f, 0x3f};
  Require(std::ranges::equal(
              CurrentThunkTail,
              std::span{current_guest_stub}.subspan(
                  Executor::Ps5Fex::CurrentHleThunkIdentityOffset -
                      CurrentThunkTail.size(),
                  CurrentThunkTail.size())),
          "Current HLE guest stub lost the retiring-JIT ThunkOp boundary");
  Require(std::ranges::equal(
              identity,
              std::span{current_guest_stub}.subspan(
                  Executor::Ps5Fex::CurrentHleThunkIdentityOffset,
                  identity.size())),
          "Current HLE guest stub did not emit the selected bridge identity");
  Require(current_emitted ==
                  Executor::Ps5Fex::CurrentHleGuestStubEmittedSize &&
              std::ranges::all_of(
                  std::span{current_guest_stub}.subspan(current_emitted),
                  [](const std::uint8_t byte) { return byte == 0xcc; }),
          "Current HLE guest stub length or trap padding changed unexpectedly");
  Require(std::search(current_guest_stub.begin(), current_guest_stub.end(),
                      CurrentThunkTail.begin(), CurrentThunkTail.end()) !=
              current_guest_stub.end(),
          "Current HLE guest stub has no ThunkOp");

  const auto emitted = Executor::Ps5Fex::BuildHleGuestStub(
      guest_stub, StubThunk, identity);
  constexpr std::array<std::uint8_t, 57> ExpectedEpilogue{
      0x48, 0x8b, 0x04, 0x24,
      0x4c, 0x8b, 0x94, 0x24, 0x80, 0x00, 0x00, 0x00,
      0x4c, 0x8b, 0x5c, 0x24, 0x48,
      0x48, 0x8b, 0x5c, 0x24, 0x50,
      0x48, 0x8b, 0x6c, 0x24, 0x58,
      0x4c, 0x8b, 0x64, 0x24, 0x60,
      0x4c, 0x8b, 0x6c, 0x24, 0x68,
      0x4c, 0x8b, 0x74, 0x24, 0x70,
      0x4c, 0x8b, 0x7c, 0x24, 0x78,
      0x48, 0x81, 0xc4, 0x90, 0x00, 0x00, 0x00,
      0x41, 0xff, 0xe2,
  };
  std::uint64_t encoded_thunk{};
  std::memcpy(&encoded_thunk, guest_stub.data() + 9, sizeof(encoded_thunk));
  Require(encoded_thunk == StubThunk,
          "HLE guest stub did not preserve its thunk identity");
  constexpr std::size_t HiddenReturnAddressOffset = 117;
  constexpr std::array<std::uint8_t, 4> HiddenReturnPrologue{
      0x48, 0x83, 0xec, 0x08};
  Require(std::ranges::equal(
              HiddenReturnPrologue,
              std::span{guest_stub}.subspan(111,
                                            HiddenReturnPrologue.size())),
          "HLE guest stub did not reserve FEX's private return slot");
  std::uint64_t encoded_epilogue{};
  std::memcpy(&encoded_epilogue,
              guest_stub.data() + HiddenReturnAddressOffset,
              sizeof(encoded_epilogue));
  Require(encoded_epilogue ==
              StubThunk + Executor::Ps5Fex::HleGuestEpilogueOffset,
          "HLE guest stub did not encode its FEX return epilogue");
  constexpr std::array<std::uint8_t, 11> HiddenReturnTail{
      0x48, 0x89, 0x04, 0x24,
      0x48, 0x8d, 0x7c, 0x24, 0x08,
      0x0f, 0x3f};
  Require(std::ranges::equal(
              HiddenReturnTail,
              std::span{guest_stub}.subspan(125,
                                            HiddenReturnTail.size())),
          "HLE guest stub did not isolate FEX's hidden return from frame[0]");
  Require(std::ranges::equal(
              identity,
              std::span{guest_stub}.subspan(
                  Executor::Ps5Fex::HleThunkIdentityOffset,
                  identity.size())),
          "HLE guest stub did not emit the selected bridge identity");
  Require(std::ranges::equal(
              ExpectedEpilogue,
              std::span{guest_stub}.subspan(
                  Executor::Ps5Fex::HleGuestEpilogueOffset,
                  ExpectedEpilogue.size())),
          "HLE guest stub omitted its FEX return epilogue");
  Require(emitted == Executor::Ps5Fex::HleGuestStubEmittedSize &&
              std::ranges::all_of(
                  std::span{guest_stub}.subspan(emitted),
                  [](const std::uint8_t byte) { return byte == 0xcc; }),
          "HLE guest stub length or trap padding changed unexpectedly");

  Lsx4FexHleCallFrameV3 call{};
  call.struct_size = sizeof(call);
  call.abi_version = LSX4_FEX_BACKEND_ABI_V3;
  call.call_id = 17;
  call.native_function = UINT64_C(0x8f0001200);
  call.guest_rsp = UINT64_C(0x8d022ff0);
  call.sret_address = UINT64_C(0x8d1000000);
  call.sret_size = 16;
  call.return_class[0] = 1;
  call.return_class[1] = 2;
  call.return_eightbyte_count = 2;
  call.sret_flags = LSX4_FEX_SRET_PRESENT;
  call.abi_descriptor_id = 99;
  for (std::size_t index = 0; index != 6; ++index) {
    call.gpr[index] = 0x100 + index;
  }
  for (std::size_t index = 0; index != 8; ++index) {
    call.xmm[index].low = 0x200 + index;
    call.xmm[index].high = 0x300 + index;
  }

  Lsx4::Translation::HleBridgeRequest bridge{};
  Require(Executor::Ps5Fex::BuildHleBridgeRequest(call, bridge),
          "valid HLE call was rejected");
  Require(bridge.function == call.native_function,
          "thunk identity was not preserved");
  Require(bridge.guest_stack == call.guest_rsp,
          "authoritative incoming RSP was not preserved");
  for (std::size_t index = 0; index != 6; ++index) {
    Require(bridge.integer_arguments[index] == call.gpr[index],
            "integer argument was not preserved");
  }
  for (std::size_t index = 0; index != 8; ++index) {
    Require(bridge.floating_arguments[index] == call.xmm[index].low,
            "XMM low lane was not preserved");
  }

  Lsx4FexHleResultFrameV3 result{};
  result.struct_size = sizeof(result);
  Executor::Ps5Fex::CompleteHleResult(call, UINT64_C(0xfedcba9876543210), 0,
                                      result);
  Require(result.struct_size == sizeof(result) &&
              result.abi_version == LSX4_FEX_BACKEND_ABI_V3 &&
              result.call_id == call.call_id &&
              result.rax == UINT64_C(0xfedcba9876543210) &&
              result.sret_address == call.sret_address &&
              result.sret_size == call.sret_size &&
              result.return_class[0] == call.return_class[0] &&
              result.return_class[1] == call.return_class[1] &&
              result.return_eightbyte_count == call.return_eightbyte_count &&
              result.sret_flags == call.sret_flags &&
              result.abi_descriptor_id == call.abi_descriptor_id &&
              result.status == LSX4_FEX_STATUS_OK,
          "HLE result contract was not completed");

  result.struct_size = sizeof(result);
  Executor::Ps5Fex::CompleteHleResult(call, UINT64_C(0xffffffff3f800000), 1,
                                      result);
  Require(result.xmm0.low == UINT64_C(0x3f800000),
          "float HLE result was not written to XMM0");
  result.struct_size = sizeof(result);
  Executor::Ps5Fex::CompleteHleResult(call, UINT64_C(0x3ff0000000000000), 2,
                                      result);
  Require(result.xmm0.low == UINT64_C(0x3ff0000000000000),
          "double HLE result was not written to XMM0");

  const auto first = Executor::Ps5Fex::StableRangeCookie(
      0x800000000, 0x4000, 5, LSX4_FEX_RANGE_EXECUTABLE);
  const auto repeat = Executor::Ps5Fex::StableRangeCookie(
      0x800000000, 0x4000, 5, LSX4_FEX_RANGE_EXECUTABLE);
  const auto distinct = Executor::Ps5Fex::StableRangeCookie(
      0x800004000, 0x4000, 5, LSX4_FEX_RANGE_EXECUTABLE);
  Require(first != 0 && first == repeat && first != distinct,
          "range cookies are not stable and range-specific");

  const std::array published{
      Lsx4FexExecutableRangeV5{
          .address = 0x10000,
          .size = 0x1000,
          .range_cookie = 1,
          .protection = 5,
          .range_flags = LSX4_FEX_RANGE_EXECUTABLE,
      },
      Lsx4FexExecutableRangeV5{
          .address = 0x20000,
          .size = 0x1000,
          .range_cookie = 2,
          .protection = 5,
          .range_flags = LSX4_FEX_RANGE_EXECUTABLE,
      },
  };
  const std::array current{
      published[1],
      Lsx4FexExecutableRangeV5{
          .address = 0x30000,
          .size = 0x1000,
          .range_cookie = 3,
          .protection = 5,
          .range_flags = LSX4_FEX_RANGE_EXECUTABLE,
      },
  };
  const auto delta =
      Executor::Ps5Fex::PlanExecutableRangeUpdate(published, current);
  Require(delta.removed.size() == 1 && delta.added.size() == 1 &&
              delta.removed[0].range_cookie == 1 &&
              delta.added[0].range_cookie == 3,
          "range update did not plan remove-before-add reconciliation");
  const auto stable_delta =
      Executor::Ps5Fex::PlanExecutableRangeUpdate(current, current);
  Require(stable_delta.removed.empty() && stable_delta.added.empty(),
          "unchanged ranges produced a spurious update");

  constexpr std::uint32_t HleProtection = 5;
  constexpr std::uint32_t HleFlags =
      LSX4_FEX_RANGE_EXECUTABLE | LSX4_FEX_RANGE_HLE_TRAMPOLINE;
  const Lsx4FexExecutableRangeV5 first_hle_stub{
      .address = UINT64_C(0x8f0000000),
      .size = 0x100,
      .range_cookie = Executor::Ps5Fex::StableRangeCookie(
          UINT64_C(0x8f0000000), 0x100, HleProtection, HleFlags),
      .protection = HleProtection,
      .range_flags = HleFlags,
  };
  const Lsx4FexExecutableRangeV5 second_hle_stub{
      .address = UINT64_C(0x8f0000100),
      .size = 0x100,
      .range_cookie = Executor::Ps5Fex::StableRangeCookie(
          UINT64_C(0x8f0000100), 0x100, HleProtection, HleFlags),
      .protection = HleProtection,
      .range_flags = HleFlags,
  };
  const std::array one_hle_stub{first_hle_stub};
  const std::array two_hle_stubs{first_hle_stub, second_hle_stub};
  const auto hle_growth =
      Executor::Ps5Fex::PlanExecutableRangeUpdate(one_hle_stub, two_hle_stubs);
  Require(hle_growth.removed.empty() && hle_growth.added.size() == 1 &&
              hle_growth.added[0].range_cookie == second_hle_stub.range_cookie,
          "binding a second immutable HLE stub was not add-only");
  return 0;
}
