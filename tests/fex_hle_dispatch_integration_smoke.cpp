// SPDX-License-Identifier: GPL-2.0-or-later

#include "executor/fex_hle_integration_seam.h"
#include "executor/fex_hle_frame_lease_abi.h"
#include "core/aerolib/hle_varargs.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <csignal>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using Core::AeroLib::AndroidX64HleAbiDescriptor;
using Core::AeroLib::AndroidX64HleRawCall;
using Core::AeroLib::AndroidX64HleRawResult;
using Core::AeroLib::AndroidX64SysVClass;

static_assert(static_cast<std::uint8_t>(AndroidX64SysVClass::NoClass) ==
              LSX4_FEX_SYSV_NO_CLASS);
static_assert(static_cast<std::uint8_t>(AndroidX64SysVClass::Integer) ==
              LSX4_FEX_SYSV_INTEGER);
static_assert(static_cast<std::uint8_t>(AndroidX64SysVClass::Sse) ==
              LSX4_FEX_SYSV_SSE);
static_assert(static_cast<std::uint8_t>(AndroidX64SysVClass::SseUp) ==
              LSX4_FEX_SYSV_SSEUP);
static_assert(static_cast<std::uint8_t>(AndroidX64SysVClass::X87) ==
              LSX4_FEX_SYSV_X87);
static_assert(static_cast<std::uint8_t>(AndroidX64SysVClass::X87Up) ==
              LSX4_FEX_SYSV_X87UP);
static_assert(static_cast<std::uint8_t>(AndroidX64SysVClass::ComplexX87) ==
              LSX4_FEX_SYSV_COMPLEX_X87);
static_assert(static_cast<std::uint8_t>(AndroidX64SysVClass::Memory) ==
              LSX4_FEX_SYSV_MEMORY);
static_assert(Core::AeroLib::AndroidX64SretPresent ==
              LSX4_FEX_SRET_PRESENT);
static_assert(Core::AeroLib::AndroidX64SretRaxReturnsAddress ==
              LSX4_FEX_SRET_RAX_RETURNS_ADDRESS);
static_assert(Core::AeroLib::AndroidX64SretHostWroteMemory ==
              LSX4_FEX_SRET_HOST_WROTE_MEMORY);
static_assert(Core::AeroLib::AndroidX64CallVariadic ==
              LSX4_FEX_CALL_VARIADIC);
static_assert(Core::AeroLib::AndroidX64CallNoReturn ==
              LSX4_FEX_CALL_NORETURN);
static_assert(
    Core::AeroLib::HleAbiDetail::MakeDescriptor<int (*)(int)>().descriptor_id ==
    Core::AeroLib::HleAbiDetail::MakeDescriptor<unsigned (*)(unsigned)>()
        .descriptor_id);
static_assert(
    Core::AeroLib::HleAbiDetail::MakeDescriptor<int (*)(int)>().descriptor_id !=
    Core::AeroLib::HleAbiDetail::MakeDescriptor<double (*)(double)>()
        .descriptor_id);

extern "C" int executor_live_get_current_hle_guest_rsp(
    std::uint64_t* guest_rsp);
extern "C" int executor_live_get_current_hle_floating_arguments(
    std::uint64_t* values, std::size_t capacity);
extern "C" bool ExecutorJitReadGuestBytes(std::uint64_t address,
                                             void* destination,
                                             std::size_t size);
extern "C" bool ExecutorJitWriteGuestBytes(std::uint64_t address,
                                              const void* source,
                                              std::size_t size);
extern "C" void executor_lsx4_android_register_guest_readable_range(
    const void* base, std::size_t size, const char* label);

namespace {

struct LargeResult {
  std::uint64_t first{};
  std::uint64_t second{};
  std::uint64_t third{};
  std::uint64_t fourth{};
};

struct GuestStack {
  alignas(16) std::array<std::uint8_t, 256> bytes{};

  explicit GuestStack(const std::uint64_t return_address) {
    std::memcpy(bytes.data(), &return_address, sizeof(return_address));
  }

  std::uint64_t address() const {
    return reinterpret_cast<std::uint64_t>(bytes.data());
  }

  template <typename T>
  void SetArgument(const std::size_t stack_offset, const T& value) {
    if (sizeof(std::uint64_t) + stack_offset + sizeof(T) > bytes.size()) {
      std::abort();
    }
    std::memcpy(bytes.data() + sizeof(std::uint64_t) + stack_offset, &value,
                sizeof(T));
  }
};

[[noreturn]] void Fail(const char* stage, const std::uint64_t actual = 0,
                       const std::uint64_t expected = 0) {
  std::fprintf(stderr,
               "FEX_HLE_DISPATCH_INTEGRATION_SMOKE FAIL stage=%s "
               "actual=0x%" PRIx64 " expected=0x%" PRIx64 "\n",
               stage, actual, expected);
  std::fflush(stderr);
  std::_Exit(1);
}

void Require(const bool condition, const char* stage,
             const std::uint64_t actual = 0,
             const std::uint64_t expected = 0) {
  if (!condition) {
    Fail(stage, actual, expected);
  }
}

template <typename Fn>
std::uint64_t FunctionAddress(Fn function) {
  return reinterpret_cast<std::uint64_t>(function);
}

void RegisterGuestRange(const void* base, const std::size_t size,
                        const char* label) {
  executor_lsx4_android_register_guest_readable_range(base, size, label);
}

void RegisterGuestStack(const GuestStack& stack) {
  RegisterGuestRange(stack.bytes.data(), stack.bytes.size(),
                     "fex-hle-integration-stack");
}

template <typename Fn>
AndroidX64HleAbiDescriptor RegisterTarget(const char* name, Fn function) {
  static constexpr AndroidX64HleAbiDescriptor descriptor =
      Core::AeroLib::HleAbiDetail::MakeDescriptor<Fn>();
  const std::uint64_t address = FunctionAddress(function);
  Require(descriptor.supported && descriptor.descriptor_id != 0 &&
              descriptor.invoke != nullptr,
          "local-descriptor-supported", descriptor.descriptor_id);
  Require(executor_lsx4_fex_hle_integration_register(
              address, name, &descriptor) == LSX4_FEX_STATUS_OK,
          "production-register", address);
  AndroidX64HleAbiDescriptor resolved{};
  Require(executor_lsx4_fex_hle_integration_resolve(address, &resolved) ==
              LSX4_FEX_STATUS_OK,
          "production-resolve", address);
  Require(resolved.descriptor_id == descriptor.descriptor_id &&
              resolved.invoke == descriptor.invoke && resolved.supported,
          "production-resolve-identity", resolved.descriptor_id,
          descriptor.descriptor_id);
  return resolved;
}

std::uint64_t g_call_id = 1;

Lsx4FexHleCallFrameV3 MakeCall(
    const std::uint64_t native_function,
    const AndroidX64HleAbiDescriptor& descriptor,
    const std::uint64_t guest_rsp) {
  Lsx4FexHleCallFrameV3 call{};
  call.struct_size = sizeof(call);
  call.abi_version = LSX4_FEX_BACKEND_ABI_V3;
  call.call_id = g_call_id++;
  call.native_function = native_function;
  call.guest_rip = UINT64_C(0x400000) + call.call_id * 16;
  call.guest_rsp = guest_rsp;
  call.return_class[0] = descriptor.return_class[0];
  call.return_class[1] = descriptor.return_class[1];
  call.return_eightbyte_count = descriptor.return_eightbyte_count;
  call.sret_flags = descriptor.sret_flags;
  call.call_flags = descriptor.call_flags;
  call.abi_descriptor_id = descriptor.descriptor_id;
  return call;
}

Lsx4FexHleResultFrameV3 MakeResult(const std::uint64_t call_id) {
  Lsx4FexHleResultFrameV3 result{};
  result.struct_size = sizeof(result);
  result.abi_version = LSX4_FEX_BACKEND_ABI_V3;
  result.call_id = call_id;
  return result;
}

int Dispatch(const Lsx4FexHleCallFrameV3& call,
             Lsx4FexHleResultFrameV3& result) {
  return executor_lsx4_fex_hle_integration_dispatch(&call, &result);
}

__attribute__((noinline, used)) std::uint64_t AddIntegers(
    const std::uint64_t first, const std::uint64_t second) {
  return first + second;
}

std::atomic_bool g_guest_thread_exit_target_called{};

__attribute__((noinline, used)) void GuestThreadExitMustBeStaged(void*) {
  g_guest_thread_exit_target_called.store(true, std::memory_order_relaxed);
}

__attribute__((noinline, used)) double AddDoubles(const double first,
                                                   const double second) {
  return first + second;
}

__attribute__((noinline, used)) double SumTenDoubles(
    const double a0, const double a1, const double a2, const double a3,
    const double a4, const double a5, const double a6, const double a7,
    const double a8, const double a9) {
  return a0 + a1 + a2 + a3 + a4 + a5 + a6 + a7 + a8 + a9;
}

__attribute__((noinline, used)) LargeResult MakeLargeResult(
    const std::uint64_t seed, const double marker) {
  return {seed, seed + 1, std::bit_cast<std::uint64_t>(marker),
          UINT64_C(0xfedcba9876543210)};
}

__attribute__((noinline, used)) std::uint64_t VariadicMarker(
    const std::uint64_t, const std::uint64_t, ...) {
  std::abort();
}

constexpr std::array<std::uint64_t, 7> kVariadicValues{
    UINT64_C(0x101), UINT64_C(0x202), UINT64_C(0x303), UINT64_C(0x404),
    UINT64_C(0x505), UINT64_C(0x606), UINT64_C(0x707)};
std::uint64_t g_expected_variadic_overflow{};
std::uint32_t g_variadic_checks{};

bool InvokeVariadicProbe(
    std::uint64_t, const AndroidX64HleRawCall& call,
    AndroidX64HleRawResult& result) {
  Common::VaCtx context{};
  if (!Core::AeroLib::BuildAndroidX64VaContext(call, 2, 0, 0, &context)) {
    return false;
  }
  bool registers_match = true;
  for (std::size_t index = 0; index < 6; ++index) {
    registers_match = registers_match &&
                      context.reg_save_area.gp[index] == kVariadicValues[index];
  }
  if (registers_match) {
    g_variadic_checks |= 1;
  }
  if (reinterpret_cast<std::uint64_t>(context.va_list.overflow_arg_area) ==
      g_expected_variadic_overflow) {
    g_variadic_checks |= 2;
  }
  std::uint64_t overflow_value{};
  if (ExecutorJitReadGuestBytes(
          reinterpret_cast<std::uint64_t>(context.va_list.overflow_arg_area),
          &overflow_value, sizeof(overflow_value)) &&
      overflow_value == kVariadicValues[6]) {
    g_variadic_checks |= 4;
  }
  if (call.call_flags == LSX4_FEX_CALL_VARIADIC &&
      call.sysv_sse_arg_count == 3) {
    g_variadic_checks |= 8;
  }
  if (g_variadic_checks != 15) {
    return false;
  }
  std::uint64_t sum{};
  for (const std::uint64_t value : kVariadicValues) {
    sum += value;
  }
  return Core::AeroLib::HleAbiDetail::StoreReturn(sum, result);
}

AndroidX64HleAbiDescriptor RegisterVariadicTarget() {
  AndroidX64HleAbiDescriptor descriptor{};
  descriptor.signature = "integration-raw-variadic-v1";
  descriptor.invoke = &InvokeVariadicProbe;
  descriptor.call_flags = Core::AeroLib::AndroidX64CallVariadic;
  descriptor.return_class[0] = LSX4_FEX_SYSV_INTEGER;
  descriptor.return_class[1] = LSX4_FEX_SYSV_NO_CLASS;
  descriptor.return_eightbyte_count = 1;
  descriptor.return_size = sizeof(std::uint64_t);
  descriptor.supported = true;
  descriptor.descriptor_id =
      Core::AeroLib::HleAbiDetail::CanonicalDescriptorId(descriptor);
  const std::uint64_t address = FunctionAddress(&VariadicMarker);
  Require(executor_lsx4_fex_hle_integration_register(
              address, "integration-raw-variadic", &descriptor) ==
              LSX4_FEX_STATUS_OK,
          "production-register-raw-variadic", address);
  AndroidX64HleAbiDescriptor resolved{};
  Require(executor_lsx4_fex_hle_integration_resolve(address, &resolved) ==
              LSX4_FEX_STATUS_OK &&
              resolved.descriptor_id == descriptor.descriptor_id &&
              resolved.invoke == descriptor.invoke && resolved.supported,
          "production-resolve-raw-variadic", resolved.descriptor_id,
          descriptor.descriptor_id);
  return resolved;
}

AndroidX64HleAbiDescriptor g_inner_descriptor{};
std::uint64_t g_inner_target{};
GuestStack g_inner_stack{UINT64_C(0x2222222222222222)};
std::uint64_t g_expected_outer_rsp{};
std::uint64_t g_expected_inner_rsp{};
std::uint64_t g_outer_xmm_marker{};
std::uint64_t g_inner_xmm_marker{};
std::uint32_t g_boundary_checks{};
volatile std::sig_atomic_t g_inside_hle{};
volatile std::sig_atomic_t g_signal_during{};
volatile std::sig_atomic_t g_signal_after{};

void SmokeSignalHandler(int) {
  if (g_inside_hle != 0) {
    ++g_signal_during;
  } else {
    ++g_signal_after;
  }
}

__attribute__((noinline, used)) std::uint64_t BoundaryInner(
    const std::uint64_t value) {
  std::uint64_t rsp{};
  std::array<std::uint64_t, 8> floating{};
  if (executor_live_get_current_hle_guest_rsp(&rsp) == 0 &&
      executor_live_get_current_hle_floating_arguments(
          floating.data(), floating.size()) ==
          static_cast<int>(floating.size()) &&
      rsp == g_expected_inner_rsp && floating[0] == g_inner_xmm_marker) {
    g_boundary_checks |= 2;
  }
  return value + 1;
}

__attribute__((noinline, used)) std::uint64_t BoundaryOuter(
    const std::uint64_t value) {
  std::uint64_t rsp{};
  std::array<std::uint64_t, 8> floating{};
  if (executor_live_get_current_hle_guest_rsp(&rsp) == 0 &&
      executor_live_get_current_hle_floating_arguments(
          floating.data(), floating.size()) ==
          static_cast<int>(floating.size()) &&
      rsp == g_expected_outer_rsp && floating[0] == g_outer_xmm_marker) {
    g_boundary_checks |= 1;
  }

  g_inside_hle = 1;
  (void)std::raise(SIGUSR2);
  g_inside_hle = 0;

  auto inner_call = MakeCall(g_inner_target, g_inner_descriptor,
                             g_inner_stack.address());
  inner_call.gpr[0] = value + 10;
  inner_call.xmm[0].low = g_inner_xmm_marker;
  auto inner_result = MakeResult(inner_call.call_id);
  if (Dispatch(inner_call, inner_result) == LSX4_FEX_STATUS_OK &&
      inner_result.status == LSX4_FEX_STATUS_OK &&
      inner_result.rax == value + 11) {
    g_boundary_checks |= 4;
  }

  rsp = 0;
  floating.fill(0);
  if (executor_live_get_current_hle_guest_rsp(&rsp) == 0 &&
      executor_live_get_current_hle_floating_arguments(
          floating.data(), floating.size()) ==
          static_cast<int>(floating.size()) &&
      rsp == g_expected_outer_rsp && floating[0] == g_outer_xmm_marker) {
    g_boundary_checks |= 8;
  }
  return value + 2;
}

struct Targets {
  AndroidX64HleAbiDescriptor integer;
  AndroidX64HleAbiDescriptor fp;
  AndroidX64HleAbiDescriptor stack;
  AndroidX64HleAbiDescriptor sret;
  AndroidX64HleAbiDescriptor variadic;
  AndroidX64HleAbiDescriptor guest_thread_exit;
  AndroidX64HleAbiDescriptor inner;
  AndroidX64HleAbiDescriptor outer;
};

Targets RegisterTargets() {
  Targets targets{
      RegisterTarget("integration-integer", &AddIntegers),
      RegisterTarget("integration-fp", &AddDoubles),
      RegisterTarget("integration-stack", &SumTenDoubles),
      RegisterTarget("integration-sret", &MakeLargeResult),
      RegisterVariadicTarget(),
      RegisterTarget("FJrT5LuUBAU", &GuestThreadExitMustBeStaged),
      RegisterTarget("integration-boundary-inner", &BoundaryInner),
      RegisterTarget("integration-boundary-outer", &BoundaryOuter),
  };
  g_inner_descriptor = targets.inner;
  g_inner_target = FunctionAddress(&BoundaryInner);
  g_expected_inner_rsp = g_inner_stack.address();
  return targets;
}

void BeginAndSealKnownGeneration(const Targets& targets) {
  const std::uint64_t generation =
      executor_lsx4_fex_hle_integration_begin_generation();
  Require(Lsx4::FexHleLease::IsCurrentManifestGeneration(generation),
          "generation-begin-current-protocol");
  const std::array<std::pair<std::uint64_t, const char*>, 8> resolved{{
      {FunctionAddress(&AddIntegers), "integration-integer"},
      {FunctionAddress(&AddDoubles), "integration-fp"},
      {FunctionAddress(&SumTenDoubles), "integration-stack"},
      {FunctionAddress(&MakeLargeResult), "integration-sret"},
      {FunctionAddress(&VariadicMarker), "integration-raw-variadic"},
      {FunctionAddress(&GuestThreadExitMustBeStaged), "FJrT5LuUBAU"},
      {FunctionAddress(&BoundaryInner), "integration-boundary-inner"},
      {FunctionAddress(&BoundaryOuter), "integration-boundary-outer"},
  }};
  (void)targets;
  for (const auto& [address, name] : resolved) {
    executor_lsx4_fex_hle_integration_mark_resolved(address, name);
  }
  const std::uint64_t probe_stub =
      executor_lsx4_fex_hle_integration_make_stub(
          FunctionAddress(&AddIntegers), "integration-integer");
  const std::uint64_t slot_size =
      executor_lsx4_fex_hle_integration_stub_slot_size();
  const std::uint64_t slab_base =
      executor_lsx4_fex_hle_integration_stub_slab_base();
  Require(probe_stub != 0 &&
              slot_size == Lsx4::FexHleLease::StubCodeBytes &&
              probe_stub >= slab_base + slot_size,
          "stub-slot-v2-geometry", probe_stub, slot_size);
  const auto* untouched_neighbor = reinterpret_cast<const std::uint8_t*>(
      probe_stub - slot_size);
  Require(untouched_neighbor[0] == 0x48 &&
              untouched_neighbor[1] == 0x31 &&
              untouched_neighbor[2] == 0xc0 &&
              untouched_neighbor[3] == 0xc3 &&
              std::all_of(untouched_neighbor + 4, untouched_neighbor + 64,
                          [](const std::uint8_t byte) {
                            return byte == 0xcc;
                          }),
          "stub-adjacent-slot-canary");
  const std::uint64_t emitted =
      executor_lsx4_fex_hle_integration_stub_max_emitted_size();
  Require(emitted != 0 &&
              emitted <= slot_size -
                             Lsx4::FexHleLease::MinimumStubSpareBytes &&
              executor_lsx4_fex_hle_integration_stub_canary_failures() == 0,
          "stub-emission-bounded-with-spare", emitted, slot_size);
  Require(executor_lsx4_fex_hle_integration_protocol_fingerprint() ==
              Lsx4::FexHleLease::ProtocolFingerprint,
          "stub-protocol-fingerprint");
  std::array<char, 512> error{};
  Require(executor_lsx4_fex_hle_integration_validate_generation(
              error.data(), error.size()) == LSX4_FEX_STATUS_OK &&
              error[0] == '\0',
          "generation-validate-known");
  Require(executor_lsx4_fex_hle_integration_seal_generation(
              error.data(), error.size()) == LSX4_FEX_STATUS_OK &&
              error[0] == '\0',
          "generation-seal-known");
}

void TestVariadicWireAndMismatch(
    const AndroidX64HleAbiDescriptor& descriptor) {
  Require(descriptor.call_flags == LSX4_FEX_CALL_VARIADIC &&
              descriptor.return_class[0] == LSX4_FEX_SYSV_INTEGER &&
              descriptor.return_eightbyte_count == 1,
          "variadic-descriptor-wire", descriptor.call_flags,
          LSX4_FEX_CALL_VARIADIC);
  GuestStack stack{UINT64_C(0x4a4a4a4a4a4a4a4a)};
  RegisterGuestStack(stack);
  stack.SetArgument(0, kVariadicValues[4]);
  stack.SetArgument(8, kVariadicValues[5]);
  stack.SetArgument(16, kVariadicValues[6]);
  g_expected_variadic_overflow = stack.address() + sizeof(std::uint64_t) + 16;
  g_variadic_checks = 0;

  auto call = MakeCall(FunctionAddress(&VariadicMarker), descriptor,
                       stack.address());
  call.gpr[0] = UINT64_C(0xaaaa);
  call.gpr[1] = UINT64_C(0xbbbb);
  for (std::size_t index = 0; index < 4; ++index) {
    call.gpr[index + 2] = kVariadicValues[index];
  }
  call.input_rax = 3;
  call.sysv_sse_arg_count = 3;
  auto result = MakeResult(call.call_id);
  Require(Dispatch(call, result) == LSX4_FEX_STATUS_OK &&
              result.status == LSX4_FEX_STATUS_OK &&
              result.rax == UINT64_C(0x1c1c) &&
              result.result_flags == 0 && g_variadic_checks == 15,
          "variadic-dispatch-normalized-overflow", result.rax,
          UINT64_C(0x1c1c));

  auto host_derived = call;
  host_derived.call_id = g_call_id++;
  host_derived.call_flags = 0;
  host_derived.sysv_sse_arg_count = 0;
  g_variadic_checks = 0;
  auto host_derived_result = MakeResult(host_derived.call_id);
  Require(Dispatch(host_derived, host_derived_result) ==
                  LSX4_FEX_STATUS_OK &&
              host_derived_result.status == LSX4_FEX_STATUS_OK &&
              host_derived_result.rax == UINT64_C(0x1c1c) &&
              host_derived_result.result_flags == 0 &&
              g_variadic_checks == 15,
          "variadic-host-derived-metadata", host_derived_result.rax,
          UINT64_C(0x1c1c));

  auto mismatch = call;
  mismatch.call_id = g_call_id++;
  mismatch.input_rax = 2;
  auto mismatch_result = MakeResult(mismatch.call_id);
  Require(Dispatch(mismatch, mismatch_result) ==
                  LSX4_FEX_STATUS_INVALID_ARGUMENT &&
              mismatch_result.status == LSX4_FEX_STATUS_INVALID_ARGUMENT,
          "variadic-al-sse-count-mismatch-rejected", mismatch_result.status,
          LSX4_FEX_STATUS_INVALID_ARGUMENT);
}

void TestIntegerWire(const AndroidX64HleAbiDescriptor& descriptor) {
  Require(descriptor.return_class[0] == LSX4_FEX_SYSV_INTEGER &&
              descriptor.return_eightbyte_count == 1 &&
              descriptor.gpr_argument_count == 2,
          "integer-descriptor-wire");
  GuestStack stack{UINT64_C(0x1111111111111111)};
  RegisterGuestStack(stack);
  auto call = MakeCall(FunctionAddress(&AddIntegers), descriptor,
                       stack.address());
  call.gpr[0] = 10;
  call.gpr[1] = 32;
  auto result = MakeResult(call.call_id);
  Require(Dispatch(call, result) == LSX4_FEX_STATUS_OK &&
              result.status == LSX4_FEX_STATUS_OK,
          "integer-dispatch", result.status);
  Require(result.rax == 42 && result.rdx == 0 &&
              result.return_class[0] == LSX4_FEX_SYSV_INTEGER &&
              result.return_class[1] == LSX4_FEX_SYSV_NO_CLASS &&
              result.return_eightbyte_count == 1 &&
              result.sret_flags == 0 && result.result_flags == 0 &&
              result.abi_descriptor_id == descriptor.descriptor_id,
          "integer-result-wire", result.rax, 42);
}

void TestGuestThreadExitStaging(
    const AndroidX64HleAbiDescriptor& descriptor) {
  Require(executor_lsx4_fex_hle_integration_make_stub(
              FunctionAddress(&GuestThreadExitMustBeStaged),
              "FJrT5LuUBAU") != 0,
          "guest-thread-exit-stub-registered");
  GuestStack stack{UINT64_C(0x3232323232323232)};
  RegisterGuestStack(stack);
  auto call = MakeCall(FunctionAddress(&GuestThreadExitMustBeStaged),
                       descriptor, stack.address());
  constexpr std::uint64_t status = UINT64_C(0x12345678abcdef00);
  call.gpr[0] = status;
  g_guest_thread_exit_target_called.store(false, std::memory_order_relaxed);
  auto result = MakeResult(call.call_id);
  Require(Dispatch(call, result) == LSX4_FEX_STATUS_OK &&
              result.status == LSX4_FEX_STATUS_OK &&
              result.result_flags == LSX4_FEX_HLE_RESULT_GUEST_EXIT &&
              result.rax == status &&
              result.abi_descriptor_id == descriptor.descriptor_id &&
              !g_guest_thread_exit_target_called.load(std::memory_order_relaxed),
          "guest-thread-exit-staged-before-native-pthread-exit", result.rax,
          status);
}

void TestFloatingWire(const AndroidX64HleAbiDescriptor& descriptor) {
  Require(descriptor.return_class[0] == LSX4_FEX_SYSV_SSE &&
              descriptor.sse_argument_count == 2,
          "fp-descriptor-wire");
  GuestStack stack{UINT64_C(0x3333333333333333)};
  RegisterGuestStack(stack);
  auto call = MakeCall(FunctionAddress(&AddDoubles), descriptor,
                       stack.address());
  call.xmm[0].low = std::bit_cast<std::uint64_t>(1.25);
  call.xmm[1].low = std::bit_cast<std::uint64_t>(2.5);
  auto result = MakeResult(call.call_id);
  Require(Dispatch(call, result) == LSX4_FEX_STATUS_OK &&
              result.status == LSX4_FEX_STATUS_OK,
          "fp-dispatch", result.status);
  Require(result.xmm0.low == std::bit_cast<std::uint64_t>(3.75) &&
              result.xmm0.high == 0 &&
              result.return_class[0] == LSX4_FEX_SYSV_SSE &&
              result.return_eightbyte_count == 1 &&
              result.result_flags == 0,
          "fp-result-wire", result.xmm0.low,
          std::bit_cast<std::uint64_t>(3.75));
}

void TestStackWire(const AndroidX64HleAbiDescriptor& descriptor) {
  Require(descriptor.sse_argument_count == 8 &&
              descriptor.stack_argument_bytes == 16,
          "stack-descriptor-wire", descriptor.stack_argument_bytes, 16);
  GuestStack stack{UINT64_C(0x4444444444444444)};
  RegisterGuestStack(stack);
  const double ninth = 9.0;
  const double tenth = 10.0;
  stack.SetArgument(0, ninth);
  stack.SetArgument(8, tenth);
  auto call = MakeCall(FunctionAddress(&SumTenDoubles), descriptor,
                       stack.address());
  for (std::size_t index = 0; index < 8; ++index) {
    call.xmm[index].low =
        std::bit_cast<std::uint64_t>(static_cast<double>(index + 1));
  }
  auto result = MakeResult(call.call_id);
  Require(Dispatch(call, result) == LSX4_FEX_STATUS_OK &&
              result.status == LSX4_FEX_STATUS_OK,
          "stack-dispatch", result.status);
  Require(result.xmm0.low == std::bit_cast<std::uint64_t>(55.0) &&
              result.return_class[0] == LSX4_FEX_SYSV_SSE &&
              result.result_flags == 0,
          "stack-result-wire", result.xmm0.low,
          std::bit_cast<std::uint64_t>(55.0));
}

void TestSretWire(const AndroidX64HleAbiDescriptor& descriptor) {
  Require(descriptor.return_class[0] == LSX4_FEX_SYSV_MEMORY &&
              descriptor.sret_flags == LSX4_FEX_SRET_PRESENT &&
              descriptor.return_size == sizeof(LargeResult),
          "sret-descriptor-wire", descriptor.return_size,
          sizeof(LargeResult));
  GuestStack stack{UINT64_C(0x5555555555555555)};
  alignas(16) LargeResult storage{};
  RegisterGuestStack(stack);
  RegisterGuestRange(&storage, sizeof(storage), "fex-hle-integration-sret");
  auto call = MakeCall(FunctionAddress(&MakeLargeResult), descriptor,
                       stack.address());
  call.gpr[0] = reinterpret_cast<std::uint64_t>(&storage);
  call.gpr[1] = UINT64_C(0x1020304050607080);
  call.xmm[0].low = UINT64_C(0x7ff8000000005678);
  call.sret_address = call.gpr[0];
  call.sret_size = sizeof(storage);
  auto result = MakeResult(call.call_id);
  Require(Dispatch(call, result) == LSX4_FEX_STATUS_OK &&
              result.status == LSX4_FEX_STATUS_OK,
          "sret-dispatch", result.status);
  constexpr std::uint8_t expected_sret_flags =
      LSX4_FEX_SRET_PRESENT | LSX4_FEX_SRET_RAX_RETURNS_ADDRESS |
      LSX4_FEX_SRET_HOST_WROTE_MEMORY;
  Require(result.rax == call.gpr[0] && result.sret_address == call.gpr[0] &&
              result.sret_size == sizeof(storage) &&
              result.return_class[0] == LSX4_FEX_SYSV_MEMORY &&
              result.sret_flags == expected_sret_flags &&
              result.result_flags == 0 &&
              storage.first == call.gpr[1] &&
              storage.second == call.gpr[1] + 1 &&
              storage.third == call.xmm[0].low &&
              storage.fourth == UINT64_C(0xfedcba9876543210),
          "sret-result-wire", result.sret_flags, expected_sret_flags);
}

void TestMismatchReject(const AndroidX64HleAbiDescriptor& integer_descriptor,
                        const AndroidX64HleAbiDescriptor& sret_descriptor) {
  GuestStack stack{UINT64_C(0x6666666666666666)};
  RegisterGuestStack(stack);
  auto call = MakeCall(FunctionAddress(&AddIntegers), integer_descriptor,
                       stack.address());
  call.gpr[0] = 1;
  call.gpr[1] = 2;
  call.abi_descriptor_id ^= UINT64_C(0x100);
  auto result = MakeResult(call.call_id);
  Require(Dispatch(call, result) == LSX4_FEX_STATUS_INVALID_ARGUMENT &&
              result.status == LSX4_FEX_STATUS_INVALID_ARGUMENT,
          "descriptor-id-mismatch-rejected", result.status,
          LSX4_FEX_STATUS_INVALID_ARGUMENT);

  alignas(16) LargeResult storage{};
  auto sret_call = MakeCall(FunctionAddress(&MakeLargeResult), sret_descriptor,
                            stack.address());
  sret_call.gpr[0] = reinterpret_cast<std::uint64_t>(&storage);
  sret_call.gpr[1] = 7;
  sret_call.sret_address = sret_call.gpr[0];
  sret_call.sret_size = sizeof(storage) - 1;
  auto sret_result = MakeResult(sret_call.call_id);
  Require(Dispatch(sret_call, sret_result) ==
                  LSX4_FEX_STATUS_INVALID_ARGUMENT &&
              sret_result.status == LSX4_FEX_STATUS_INVALID_ARGUMENT,
          "sret-metadata-mismatch-rejected", sret_result.status,
          LSX4_FEX_STATUS_INVALID_ARGUMENT);
}

void TestGuestAccessDeny(
    const AndroidX64HleAbiDescriptor& stack_descriptor,
    const AndroidX64HleAbiDescriptor& sret_descriptor) {
  std::uint64_t scratch{};
  constexpr std::uint64_t denied = UINT64_C(0x8000);
  Require(!ExecutorJitReadGuestBytes(denied, &scratch, sizeof(scratch)),
          "production-read-deny");
  Require(!ExecutorJitWriteGuestBytes(denied, &scratch, sizeof(scratch)),
          "production-write-deny");
  constexpr std::uint64_t unmapped = UINT64_C(0x100000);
  Require(!ExecutorJitReadGuestBytes(unmapped, &scratch, sizeof(scratch)),
          "production-unmapped-read-deny");
  Require(!ExecutorJitWriteGuestBytes(unmapped, &scratch, sizeof(scratch)),
          "production-unmapped-write-deny");

  AndroidX64HleRawCall stack_call{};
  stack_call.native_function = FunctionAddress(&SumTenDoubles);
  stack_call.guest_rsp = denied - sizeof(std::uint64_t);
  for (std::size_t index = 0; index < 8; ++index) {
    stack_call.xmm[index].low =
        std::bit_cast<std::uint64_t>(static_cast<double>(index + 1));
  }
  AndroidX64HleRawResult stack_result{};
  Require(executor_lsx4_fex_hle_integration_invoke_raw(
              &stack_call, &stack_result) == LSX4_FEX_STATUS_BACKEND_FAILURE,
          "typed-stack-read-deny", stack_descriptor.descriptor_id);

  AndroidX64HleRawCall sret_call{};
  sret_call.native_function = FunctionAddress(&MakeLargeResult);
  sret_call.gpr[0] = denied;
  sret_call.gpr[1] = 9;
  sret_call.xmm[0].low = std::bit_cast<std::uint64_t>(1.0);
  AndroidX64HleRawResult sret_result{};
  Require(executor_lsx4_fex_hle_integration_invoke_raw(
              &sret_call, &sret_result) == LSX4_FEX_STATUS_BACKEND_FAILURE,
          "typed-sret-write-deny", sret_descriptor.descriptor_id);
}

void TestNestedBoundaryAndSignal(
    const AndroidX64HleAbiDescriptor& outer_descriptor) {
  struct sigaction action {};
  action.sa_handler = SmokeSignalHandler;
  sigemptyset(&action.sa_mask);
  struct sigaction previous {};
  Require(sigaction(SIGUSR2, &action, &previous) == 0,
          "signal-handler-install");

  GuestStack outer_stack{UINT64_C(0x7777777777777777)};
  RegisterGuestStack(outer_stack);
  RegisterGuestStack(g_inner_stack);
  g_expected_outer_rsp = outer_stack.address();
  g_outer_xmm_marker = UINT64_C(0x7ff8000000001111);
  g_inner_xmm_marker = UINT64_C(0x8000000000000000);
  g_boundary_checks = 0;
  g_inside_hle = 0;
  g_signal_during = 0;
  g_signal_after = 0;

  auto call = MakeCall(FunctionAddress(&BoundaryOuter), outer_descriptor,
                       outer_stack.address());
  call.gpr[0] = 40;
  call.xmm[0].low = g_outer_xmm_marker;
  auto result = MakeResult(call.call_id);
  Require(Dispatch(call, result) == LSX4_FEX_STATUS_OK &&
              result.status == LSX4_FEX_STATUS_OK && result.rax == 42,
          "nested-boundary-dispatch", result.rax, 42);
  Require(g_boundary_checks == 15, "nested-boundary-restore",
          g_boundary_checks, 15);
  std::array<std::uint64_t, 8> floating{};
  Require(executor_live_get_current_hle_floating_arguments(
              floating.data(), floating.size()) == -1,
          "boundary-cleared-after-return");
  Require(g_signal_during == 1, "signal-during-return", g_signal_during, 1);
  (void)std::raise(SIGUSR2);
  Require(g_signal_after == 1, "signal-after-return", g_signal_after, 1);
  Require(sigaction(SIGUSR2, &previous, nullptr) == 0,
          "signal-handler-restore");
}

void TestGenerationStaleReset(const Targets& targets) {
  std::array<char, 512> error{};
  const std::uint64_t stale_generation =
      executor_lsx4_fex_hle_integration_begin_generation();
  executor_lsx4_fex_hle_integration_mark_resolved(
      UINT64_C(0xdecafbad00001000), "stale-negative");
  Require(executor_lsx4_fex_hle_integration_validate_generation(
              error.data(), error.size()) == LSX4_FEX_STATUS_ABI_MISMATCH &&
              std::string_view(error.data()).find("stale-negative:missing") !=
                  std::string_view::npos,
          "generation-negative-detected");

  error.fill(0);
  const std::uint64_t clean_generation =
      executor_lsx4_fex_hle_integration_begin_generation();
  Require(clean_generation > stale_generation, "generation-advanced",
          clean_generation, stale_generation + 1);
  executor_lsx4_fex_hle_integration_mark_resolved(
      FunctionAddress(&AddIntegers), "integration-integer-clean");
  Require(executor_lsx4_fex_hle_integration_seal_generation(
              error.data(), error.size()) == LSX4_FEX_STATUS_OK &&
              error[0] == '\0',
          "generation-stale-reset-sealed", targets.integer.descriptor_id);
}

void TestManifestClosingAndPersistentAbi(const Targets& targets) {
  constexpr std::uint64_t kFakeTargetBase = UINT64_C(0x5a0000000000);
  constexpr std::size_t kRaceTargets = 32;
  const auto register_fake = [&](const std::uint64_t address,
                                 const char* name) {
    Require(executor_lsx4_fex_hle_integration_register(
                address, name, &targets.integer) == LSX4_FEX_STATUS_OK,
            "manifest-register-fake", address);
  };

  (void)executor_lsx4_fex_hle_integration_begin_generation();
  const std::uint64_t cached_target = kFakeTargetBase;
  register_fake(cached_target, "manifest-cache-hit");
  const std::uint64_t cached_stub =
      executor_lsx4_fex_hle_integration_make_stub(
          cached_target, "manifest-cache-hit");
  Require(cached_stub != 0, "manifest-cache-prime");

  std::array<std::uint64_t, kRaceTargets> race_results{};
  std::atomic<std::size_t> ready{};
  std::atomic<bool> start{};
  std::vector<std::thread> workers;
  workers.reserve(kRaceTargets + 1);
  for (std::size_t index = 0; index < kRaceTargets; ++index) {
    workers.emplace_back([&, index] {
      const std::uint64_t target =
          kFakeTargetBase + UINT64_C(0x1000) * (index + 1);
      const std::string name = "manifest-resolution-race-" +
                               std::to_string(index);
      register_fake(target, name.c_str());
      ready.fetch_add(1, std::memory_order_release);
      while (!start.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      race_results[index] =
          executor_lsx4_fex_hle_integration_make_stub(target, name.c_str());
    });
  }

  std::uint64_t low_race_result{};
  workers.emplace_back([&] {
    ready.fetch_add(1, std::memory_order_release);
    while (!start.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    low_race_result =
        executor_lsx4_fex_hle_integration_test_make_nid_stub(
            "+ADiO6FQvEA");
  });
  while (ready.load(std::memory_order_acquire) != kRaceTargets + 1) {
    std::this_thread::yield();
  }
  start.store(true, std::memory_order_release);
  std::this_thread::yield();
  std::array<char, 512> error{};
  Require(executor_lsx4_fex_hle_integration_seal_generation(
              error.data(), error.size()) == LSX4_FEX_STATUS_OK,
          "manifest-racing-seal");
  for (auto& worker : workers) {
    worker.join();
  }

  Require(executor_lsx4_fex_hle_integration_make_stub(
              cached_target, "manifest-cache-hit") == cached_stub,
          "manifest-cache-hit-after-seal");
  const std::uint64_t late_target =
      kFakeTargetBase + UINT64_C(0x1000) * (kRaceTargets + 2);
  register_fake(late_target, "manifest-late-new-target");
  Require(executor_lsx4_fex_hle_integration_make_stub(
              late_target, "manifest-late-new-target") == 0,
          "manifest-new-target-rejected-after-seal");
  const std::uint64_t low_after_seal =
      executor_lsx4_fex_hle_integration_test_make_nid_stub(
          "+ADiO6FQvEA");
  Require((low_race_result == 0 && low_after_seal == 0) ||
              (low_race_result != 0 && low_after_seal == low_race_result),
          "manifest-low-reserve-race-or-cache", low_after_seal,
          low_race_result);

  // A leased slot is a process-lifetime ABI obligation.  Beginning a fresh
  // generation must not forget an invalid target merely because it is no
  // longer present in the current generation's resolved-target set.
  (void)executor_lsx4_fex_hle_integration_begin_generation();
  const std::uint64_t missing_target =
      kFakeTargetBase + UINT64_C(0x1000) * (kRaceTargets + 3);
  Require(executor_lsx4_fex_hle_integration_make_stub(
              missing_target, "manifest-persistent-missing") != 0,
          "manifest-missing-abi-stub-created");
  error.fill(0);
  Require(executor_lsx4_fex_hle_integration_seal_generation(
              error.data(), error.size()) == LSX4_FEX_STATUS_ABI_MISMATCH &&
              std::string_view(error.data()).find(
                  "manifest-persistent-missing:missing") !=
                  std::string_view::npos,
          "manifest-missing-abi-first-reject");
  (void)executor_lsx4_fex_hle_integration_begin_generation();
  error.fill(0);
  Require(executor_lsx4_fex_hle_integration_seal_generation(
              error.data(), error.size()) == LSX4_FEX_STATUS_ABI_MISMATCH &&
              std::string_view(error.data()).find(
                  "manifest-persistent-missing:missing") !=
                  std::string_view::npos,
          "manifest-missing-abi-not-forgotten");
}

} // namespace

int main(const int argc, char** argv) {
  const Targets targets = RegisterTargets();
  if (argc == 2 && std::string_view(argv[1]) == "--staged-exit-only") {
    TestGuestThreadExitStaging(targets.guest_thread_exit);
    std::printf(
        "FEX_HLE_DISPATCH_INTEGRATION_SMOKE PASS staged_pthread_exit=1 "
        "native_exit_called=0\n");
    std::fflush(stdout);
    std::_Exit(0);
  }
  BeginAndSealKnownGeneration(targets);
  TestIntegerWire(targets.integer);
  TestGuestThreadExitStaging(targets.guest_thread_exit);
  TestFloatingWire(targets.fp);
  TestStackWire(targets.stack);
  TestSretWire(targets.sret);
  TestVariadicWireAndMismatch(targets.variadic);
  TestMismatchReject(targets.integer, targets.sret);
  TestGuestAccessDeny(targets.stack, targets.sret);
  TestNestedBoundaryAndSignal(targets.outer);
  TestGenerationStaleReset(targets);
  TestManifestClosingAndPersistentAbi(targets);
  std::printf(
      "FEX_HLE_DISPATCH_INTEGRATION_SMOKE PASS production_registry=1 "
      "dispatch=1 integer=1 fp=1 stack=1 sret=1 wire_constants=1 "
      "variadic=2 normalized_overflow=1 result_flags=1 staged_pthread_exit=1 "
      "mismatch_reject=3 "
      "guest_deny=6 nested_restore=1 "
      "signal_during_after=1 generation_reset=1 manifest_closing=1 "
      "manifest_cache_hit=1 manifest_persistent_abi=1\n");
  std::fflush(stdout);
  std::_Exit(0);
}
