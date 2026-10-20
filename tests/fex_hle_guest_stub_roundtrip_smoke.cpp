// SPDX-License-Identifier: GPL-2.0-or-later

#include "executor/dynamic_translation/hle_thunk_identity.h"
#include "executor/fex_backend_abi_v3.h"
#include "executor/fex_hle_frame_lease_abi.h"
#include "executor/fex_hle_integration_seam.h"

#include <array>
#include <atomic>
#include <bit>
#include <cerrno>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <string>

#include <dlfcn.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

extern "C" void executor_lsx4_android_register_guest_readable_range(
    const void* base, std::size_t size, const char* label);
extern "C" bool ExecutorJitReadGuestBytes(
    std::uint64_t address, void* destination, std::size_t size);
extern "C" bool ExecutorJitWriteGuestBytes(
    std::uint64_t address, const void* source, std::size_t size);

namespace {

constexpr std::size_t kPageSize = 4096;
constexpr std::size_t kStackSize = 64 * 1024;
constexpr std::size_t kHleStackScratchSize = 0x1000;
constexpr std::uint64_t kHleSafePoolSize = 256 * 1024 * 1024;
constexpr std::uint64_t kHleSafeSlotSize = 0x1000;
constexpr std::uint64_t kHleScratchCanary = UINT64_C(0x5343524154434847);
constexpr std::size_t kSignalRedirectOffset = 0x700;
constexpr std::size_t kSignalHandlerOffset = 0x800;
constexpr std::size_t kNestedSignalHandlerOffset = 0x900;
constexpr std::size_t kNestedCallbackOffset = 0xa00;
constexpr std::uint64_t kGuestExitSyscall = UINT64_C(0x5ad00002);
constexpr std::uint64_t kExpectedExit = UINT64_C(0x5a);
constexpr std::uint64_t kSignalRedirectExit = UINT64_C(0x5b);
constexpr std::uint64_t kGuestThread = UINT64_C(0x4753545542524944);
constexpr std::int32_t kQueuedSignal = 12;
constexpr std::int32_t kNestedQueuedSignal = 13;
constexpr std::uint32_t kOrbisSaSigInfo = 4;
constexpr std::uint64_t kNestedCallbackId = UINT64_C(0x4e45535443414c4c);
constexpr std::uint64_t kNestedDriverMagic = UINT64_C(0x4e45535445444f4b);
constexpr std::uint64_t kIntegerFirst = UINT64_C(0x1111222233334444);
constexpr std::uint64_t kIntegerSecond = UINT64_C(0x0102030405060708);
constexpr std::uint64_t kIntegerExpected = kIntegerFirst + kIntegerSecond;
constexpr double kFpFirst = 1.25;
constexpr double kFpSecond = 2.5;
constexpr double kFpExpected = 3.75;
constexpr std::uint64_t kSretSeed = UINT64_C(0x1020304050607080);
constexpr std::uint64_t kSretMarker = UINT64_C(0x7ff8000000005678);
constexpr std::uint64_t kSretTail = UINT64_C(0xfedcba9876543210);
constexpr std::uint64_t kVariadicFirst = UINT64_C(0x4012000000000000);
constexpr std::uint64_t kVariadicSecond = UINT64_C(0x4016000000000000);
constexpr std::uint64_t kVariadicResult = UINT64_C(0x5641524941444943);

constexpr std::uint64_t kRequiredCapabilities =
    LSX4_FEX_CAP_MAPPED_ENTRY_V5 | LSX4_FEX_CAP_HLE_FRAME_V3 |
    LSX4_FEX_CAP_STRONG_MEMORY | LSX4_FEX_CAP_GUEST_SIGNALS |
    LSX4_FEX_CAP_GUEST_TLS | LSX4_FEX_CAP_GUEST_CALLBACKS |
    LSX4_FEX_CAP_GUEST_FUTEX_SCHEDULING |
    LSX4_FEX_CAP_SELF_MODIFYING_CODE |
    LSX4_FEX_CAP_EXECUTABLE_RANGE_V5 |
    LSX4_FEX_CAP_PROCESS_LIFETIME |
    LSX4_FEX_CAP_SINGLE_SIGNAL_OWNER;

struct LargeResult {
  std::uint64_t first{};
  std::uint64_t second{};
  std::uint64_t third{};
  std::uint64_t fourth{};
};

struct Probe {
  std::uint64_t before_rsp{};
  std::uint64_t after_integer_rsp{};
  std::uint64_t after_fp_rsp{};
  std::uint64_t after_variadic_rsp{};
  std::uint64_t after_sret_rsp{};
  std::uint64_t integer_result{};
  std::uint64_t fp_result{};
  std::uint64_t variadic_result{};
  std::uint64_t sret_rax{};
  std::uint64_t integer_continuation_rip{};
  std::uint64_t fp_continuation_rip{};
  std::uint64_t variadic_continuation_rip{};
  std::uint64_t sret_continuation_rip{};
  std::uint64_t signal_handler_count{};
  std::uint64_t signal_seen_at_continuation{};
  std::uint64_t signal_redirect_seen{};
  std::uint64_t signal_context_rax{};
  std::uint64_t signal_context_rip{};
  std::uint64_t signal_context_rsp{};
  std::uint64_t nested_callback_count{};
  std::uint64_t nested_signal_handler_count{};
  std::uint64_t nested_signal_seen_inside_callback{};
  std::uint64_t nested_driver_result{};
  std::uint64_t nested_queue_result{};
  LargeResult sret{};
};

struct GuestImage {
  std::uint8_t* code{};
  std::uint8_t* stack{};
  std::uint8_t* low_stack{};
  std::uint8_t* high7_stack{};
  std::uint8_t* tls{};
  Probe* probe{};
  std::uint64_t integer_stub{};
  std::uint64_t fp_stub{};
  std::uint64_t variadic_stub{};
  std::uint64_t sret_stub{};
  std::uint64_t nested_driver_stub{};
  std::uint64_t nested_queue_stub{};
  std::uint64_t integer_call_return{};
  std::uint64_t integer_continuation{};
  std::uint64_t fp_continuation{};
  std::uint64_t variadic_continuation{};
  std::uint64_t sret_continuation{};
  std::array<Lsx4FexExecutableRangeV5, 2> ranges{};
  Lsx4FexExecutableRangeSnapshotV5 range_snapshot{};
};

const Lsx4FexBackendApiV3* g_api{};
Lsx4FexThreadIdentityV3 g_main_identity{};
std::atomic<int> g_hle_entered{};
std::atomic<int> g_signal_queue_done{};
std::atomic<int> g_signal_queue_status{LSX4_FEX_STATUS_INVALID_STATE};
volatile sig_atomic_t g_host_sigurg_seen{};
std::atomic<std::uint32_t> g_integer_calls{};
std::atomic<std::uint32_t> g_fp_calls{};
std::atomic<std::uint32_t> g_variadic_calls{};
std::atomic<std::uint32_t> g_sret_calls{};
std::atomic<std::uint32_t> g_fatal_calls{};
std::atomic<std::uint64_t> g_integer_guest_rip{};
std::atomic<std::uint64_t> g_nested_guest_function{};
std::atomic<std::int32_t> g_nested_callback_api_status{
    LSX4_FEX_STATUS_INVALID_STATE};
std::atomic<std::int32_t> g_nested_callback_result_status{
    LSX4_FEX_STATUS_INVALID_STATE};
std::atomic<std::int32_t> g_nested_queue_status{
    LSX4_FEX_STATUS_INVALID_STATE};
std::atomic<std::uint32_t> g_nested_driver_calls{};
std::atomic<std::uint32_t> g_nested_queue_calls{};
std::atomic<std::uint64_t> g_pool_wrap_probe_stub{};
std::atomic<std::uint32_t> g_pool_wrap_forced{};
bool g_plain_hle_mode{};

Lsx4FexCpuidResultV6 QueryPs4Cpuid(const void*, std::uint32_t leaf,
                                   std::uint32_t subleaf) {
  (void)subleaf;
  switch (leaf) {
  case 0:
    return {.eax = UINT32_C(7),
            .ebx = UINT32_C(0x68747541),
            .ecx = UINT32_C(0x444d4163),
            .edx = UINT32_C(0x69746e65)};
  case 1:
    return {.eax = UINT32_C(0x00610f01),
            .ebx = UINT32_C(0x00000800),
            .ecx = UINT32_C(0x3ed80201),
            .edx = UINT32_C(0x07808131)};
  default:
    return {};
  }
}

std::uint64_t QueryPs4Xcr(const void*, std::uint32_t index) {
  return index == 0 ? UINT64_C(7) : 0;
}

std::int32_t AcquireStableSmokeGuestMemoryLease(
    void*, std::uint64_t guest_address, std::uint64_t size,
    std::uint32_t access_flags, std::uint64_t* lease_token) {
  constexpr std::uint32_t permission_flags =
      LSX4_FEX_GUEST_MEMORY_ACCESS_READ |
      LSX4_FEX_GUEST_MEMORY_ACCESS_WRITE;
  constexpr std::uint32_t valid_flags =
      permission_flags |
      LSX4_FEX_GUEST_MEMORY_ACCESS_NO_HOST_REENTRY;
  if (lease_token == nullptr || guest_address == 0 || size == 0 ||
      guest_address > UINT64_MAX - size ||
      (access_flags & permission_flags) == 0 ||
      (access_flags & ~valid_flags) != 0) {
    return LSX4_FEX_STATUS_INVALID_ARGUMENT;
  }
  // Every guest mapping in this smoke remains alive until process exit.  The
  // token therefore only proves the acquire/release protocol; production uses
  // the executor's real range lease to pin mutable title mappings.
  *lease_token = guest_address;
  return LSX4_FEX_STATUS_OK;
}

void ReleaseStableSmokeGuestMemoryLease(void*, std::uint64_t) {}

[[noreturn]] void Fail(const char* stage, std::uint64_t actual = 0,
                       std::uint64_t expected = 0) {
  std::fprintf(stderr,
               "FEX_HLE_GUEST_STUB_ROUNDTRIP FAIL stage=%s actual=0x%" PRIx64
               " expected=0x%" PRIx64 " errno=%d (%s)\n",
               stage, actual, expected, errno, std::strerror(errno));
  std::fflush(stderr);
  std::_Exit(1);
}

void Require(bool condition, const char* stage, std::uint64_t actual = 0,
             std::uint64_t expected = 0) {
  if (!condition) {
    Fail(stage, actual, expected);
  }
}

void* Allocate(std::size_t size, int protection) {
  void* memory = mmap(nullptr, size, protection,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  return memory == MAP_FAILED ? nullptr : memory;
}

void* AllocateAt(std::uint64_t address, std::size_t size) {
  void* memory = mmap(reinterpret_cast<void*>(address), size,
                      PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE |
                          MAP_FIXED_NOREPLACE,
                      -1, 0);
  if (memory != reinterpret_cast<void*>(address)) {
    if (memory != MAP_FAILED) {
      munmap(memory, size);
    }
    return nullptr;
  }
  return memory;
}

std::uint64_t InitialGuestRsp(const std::uint8_t* stack) {
  return (reinterpret_cast<std::uint64_t>(stack) + kStackSize) &
         ~UINT64_C(15);
}

std::uint64_t HleScratchUnderflowCanaryAddress(const std::uint8_t* stack) {
  // run_mapped_entry enters with (top - 8).  The guest probe below uses a
  // standard 8-byte prologue so RSP is 16-byte aligned immediately before
  // CALL.  At stub entry S=(top-24), the legacy hidden CALL wrote to
  // S-0x1000-8, just below its claimed 4-KiB scratch interval.
  return InitialGuestRsp(stack) - 8 - 8 - 8 - kHleStackScratchSize - 8;
}

void ArmHleScratchUnderflowCanary(std::uint8_t* stack) {
  const std::uint64_t address = HleScratchUnderflowCanaryAddress(stack);
  std::memcpy(reinterpret_cast<void*>(address), &kHleScratchCanary,
              sizeof(kHleScratchCanary));
}

void RequireHleScratchUnderflowCanary(const std::uint8_t* stack,
                                      const char* stage) {
  std::uint64_t actual{};
  const std::uint64_t address = HleScratchUnderflowCanaryAddress(stack);
  std::memcpy(&actual, reinterpret_cast<const void*>(address), sizeof(actual));
  Require(actual == kHleScratchCanary, stage, actual, kHleScratchCanary);
}

std::string DirectoryOf(const char* path) {
  const std::string value = path != nullptr ? path : "";
  const std::size_t slash = value.find_last_of("/\\");
  return slash == std::string::npos ? std::string{"."}
                                    : value.substr(0, slash);
}

extern "C" __attribute__((visibility("default"))) int
executor_lsx4_android_runtime_jit_active() {
  // The production generator chooses the FEX marker whenever the current
  // backend is not the independent LSX4 JIT.  Exporting this from the smoke
  // executable models that backend state without mutating runtime globals.
  return 0;
}

__attribute__((noinline, used)) std::uint64_t RoundtripInteger(
    std::uint64_t first, std::uint64_t second) {
  g_integer_calls.fetch_add(1, std::memory_order_relaxed);
  if (g_plain_hle_mode) {
    return first + second;
  }
  g_hle_entered.store(1, std::memory_order_release);
  for (std::uint32_t spin = 0; spin < 500000; ++spin) {
    if (g_signal_queue_done.load(std::memory_order_acquire) != 0 &&
        g_host_sigurg_seen != 0) {
      return first + second;
    }
    (void)syscall(SYS_sched_yield);
  }
  return UINT64_C(0xdeadbeefdeadbeef);
}

__attribute__((noinline, used)) double RoundtripFp(double first,
                                                   double second) {
  g_fp_calls.fetch_add(1, std::memory_order_relaxed);
  return first + second;
}

__attribute__((noinline, used)) double RoundtripStackFp(
    double a0, double a1, double a2, double a3, double a4, double a5,
    double a6, double a7, double a8, double a9) {
  return a0 + a1 + a2 + a3 + a4 + a5 + a6 + a7 + a8 + a9;
}

__attribute__((noinline, used)) LargeResult RoundtripSret(
    std::uint64_t seed, double marker) {
  g_sret_calls.fetch_add(1, std::memory_order_relaxed);
  return {seed, seed + 1, std::bit_cast<std::uint64_t>(marker), kSretTail};
}

__attribute__((noinline, used)) void RoundtripVariadicMarker() {}

__attribute__((noinline, used)) std::uint64_t QueueNestedSignal() {
  g_nested_queue_calls.fetch_add(1, std::memory_order_relaxed);
  if (g_api == nullptr || g_api->queue_guest_signal == nullptr) {
    g_nested_queue_status.store(LSX4_FEX_STATUS_INVALID_STATE,
                                std::memory_order_release);
    return UINT64_MAX;
  }
  const Lsx4FexGuestSignalDeliveryV3 delivery{
      .struct_size = sizeof(Lsx4FexGuestSignalDeliveryV3),
      .abi_version = LSX4_FEX_BACKEND_ABI_V3,
      .target = g_main_identity,
      .signum = kNestedQueuedSignal,
      .delivery_flags = 0,
      .explicit_rip = 0,
      .explicit_rsp = 0,
      .explicit_context = nullptr,
  };
  const std::int32_t status =
      g_api->queue_guest_signal(g_api->backend_context, &delivery);
  g_nested_queue_status.store(status, std::memory_order_release);
  return static_cast<std::uint64_t>(static_cast<std::uint32_t>(status));
}

bool ForceNextPoolCandidateToLiveOuterSlot() {
  const std::uint64_t stub_address =
      g_pool_wrap_probe_stub.load(std::memory_order_acquire);
  if (stub_address == 0) {
    return false;
  }
  const auto* stub = reinterpret_cast<const std::uint8_t*>(stub_address);
  for (std::size_t offset = 0;
       offset + 21 <= Lsx4::FexHleLease::StubCodeBytes; ++offset) {
    if (stub[offset] != 0x48 || stub[offset + 1] != 0xb8 ||
        stub[offset + 10] != 0x41 || stub[offset + 11] != 0xba ||
        stub[offset + 16] != 0xf0 || stub[offset + 17] != 0x4c ||
        stub[offset + 18] != 0x0f || stub[offset + 19] != 0xc1 ||
        stub[offset + 20] != 0x10) {
      continue;
    }
    std::uint32_t slot_size{};
    std::uint64_t cursor_address{};
    std::memcpy(&slot_size, stub + offset + 12, sizeof(slot_size));
    std::memcpy(&cursor_address, stub + offset + 2,
                sizeof(cursor_address));
    if (slot_size != kHleSafeSlotSize || cursor_address == 0 ||
        (cursor_address & (alignof(std::uint64_t) - 1)) != 0) {
      return false;
    }
    auto& cursor = *reinterpret_cast<std::uint64_t*>(cursor_address);
    std::atomic_ref<std::uint64_t>(cursor).fetch_add(
        kHleSafePoolSize - kHleSafeSlotSize, std::memory_order_seq_cst);
    g_pool_wrap_forced.fetch_add(1, std::memory_order_release);
    return true;
  }
  return false;
}

__attribute__((noinline, used)) std::uint64_t RunNestedGuestCallback() {
  g_nested_driver_calls.fetch_add(1, std::memory_order_relaxed);
  const std::uint64_t guest_function =
      g_nested_guest_function.load(std::memory_order_acquire);
  if (g_api == nullptr || g_api->run_guest_callback == nullptr ||
      guest_function == 0) {
    g_nested_callback_api_status.store(LSX4_FEX_STATUS_INVALID_STATE,
                                       std::memory_order_release);
    return UINT64_MAX;
  }
  if (!ForceNextPoolCandidateToLiveOuterSlot()) {
    g_nested_callback_api_status.store(LSX4_FEX_STATUS_INVALID_STATE,
                                       std::memory_order_release);
    return UINT64_MAX;
  }
  const Lsx4FexGuestCallbackRequestV3 request{
      .struct_size = sizeof(Lsx4FexGuestCallbackRequestV3),
      .abi_version = LSX4_FEX_BACKEND_ABI_V3,
      .callback_id = kNestedCallbackId,
      .guest_function = guest_function,
      .guest_rsp = 0,
      .gpr = {},
      .xmm = {},
      .sret_address = 0,
      .sret_size = 0,
      .return_class = {LSX4_FEX_SYSV_INTEGER, LSX4_FEX_SYSV_NO_CLASS},
      .return_eightbyte_count = 1,
      .sret_flags = 0,
      .call_flags = 0,
      .sysv_sse_arg_count = 0,
      .abi_descriptor_id = kNestedCallbackId,
      .input_rax = 0,
  };
  Lsx4FexGuestCallbackResultV3 result{
      .struct_size = sizeof(Lsx4FexGuestCallbackResultV3),
      .abi_version = LSX4_FEX_BACKEND_ABI_V3,
      .call_id = kNestedCallbackId,
  };
  const std::int32_t status =
      g_api->run_guest_callback(g_api->backend_context, &request, &result);
  g_nested_callback_api_status.store(status, std::memory_order_release);
  g_nested_callback_result_status.store(result.status,
                                         std::memory_order_release);
  return status == LSX4_FEX_STATUS_OK &&
                 result.status == LSX4_FEX_STATUS_OK &&
                 result.call_id == kNestedCallbackId &&
                 result.abi_descriptor_id == kNestedCallbackId
             ? kNestedDriverMagic
             : UINT64_MAX;
}

bool InvokeRoundtripVariadic(
    std::uint64_t, const Core::AeroLib::AndroidX64HleRawCall& call,
    Core::AeroLib::AndroidX64HleRawResult& result) {
  if (call.call_flags != Core::AeroLib::AndroidX64CallVariadic ||
      call.sysv_sse_arg_count != 8 || (call.input_rax & UINT64_C(0xff)) != 8 ||
      call.xmm[0].low != kVariadicFirst ||
      call.xmm[1].low != kVariadicSecond) {
    return false;
  }
  g_variadic_calls.fetch_add(1, std::memory_order_relaxed);
  return Core::AeroLib::HleAbiDetail::StoreReturn(kVariadicResult, result);
}

Core::AeroLib::AndroidX64HleAbiDescriptor VariadicDescriptor() {
  Core::AeroLib::AndroidX64HleAbiDescriptor descriptor{};
  descriptor.signature = "guest-roundtrip-variadic-upper-bound-v1";
  descriptor.invoke = &InvokeRoundtripVariadic;
  descriptor.call_flags = Core::AeroLib::AndroidX64CallVariadic;
  descriptor.return_class[0] =
      static_cast<std::uint8_t>(Core::AeroLib::AndroidX64SysVClass::Integer);
  descriptor.return_class[1] =
      static_cast<std::uint8_t>(Core::AeroLib::AndroidX64SysVClass::NoClass);
  descriptor.return_eightbyte_count = 1;
  descriptor.return_size = sizeof(std::uint64_t);
  descriptor.supported = true;
  descriptor.descriptor_id =
      Core::AeroLib::HleAbiDetail::CanonicalDescriptorId(descriptor);
  return descriptor;
}

class ImageBuilder {
public:
  explicit ImageBuilder(std::uint8_t* code) : code_{code} {}

  void Seek(std::size_t offset) {
    Require(offset < kPageSize, "image-seek", offset, kPageSize);
    cursor_ = offset;
  }

  std::size_t cursor() const { return cursor_; }

  void U8(std::uint8_t value) {
    Require(cursor_ < kPageSize, "image-u8", cursor_, kPageSize);
    code_[cursor_++] = value;
  }

  void U32(std::uint32_t value) {
    Require(cursor_ + sizeof(value) <= kPageSize, "image-u32");
    std::memcpy(code_ + cursor_, &value, sizeof(value));
    cursor_ += sizeof(value);
  }

  void U64(std::uint64_t value) {
    Require(cursor_ + sizeof(value) <= kPageSize, "image-u64");
    std::memcpy(code_ + cursor_, &value, sizeof(value));
    cursor_ += sizeof(value);
  }

  void Bytes(std::initializer_list<std::uint8_t> values) {
    for (const std::uint8_t value : values) {
      U8(value);
    }
  }

  void MovAbsRax(std::uint64_t value) {
    Bytes({0x48, 0xb8});
    U64(value);
  }

  void MovAbsR10(std::uint64_t value) {
    Bytes({0x49, 0xba});
    U64(value);
  }

  void MovAbsR11(std::uint64_t value) {
    Bytes({0x49, 0xbb});
    U64(value);
  }

  void CallAbsolute(std::uint64_t target) {
    MovAbsR11(target);
    Bytes({0x41, 0xff, 0xd3});
  }

  void StoreRsp(std::uint64_t destination) {
    MovAbsRax(destination);
    Bytes({0x48, 0x89, 0x20});
  }

  void StoreRax(std::uint64_t destination) {
    MovAbsR10(destination);
    Bytes({0x49, 0x89, 0x02});
  }

  void StoreXmm0(std::uint64_t destination) {
    MovAbsRax(destination);
    Bytes({0x66, 0x0f, 0xd6, 0x00});
  }

  std::uint64_t CaptureRip(std::uint64_t code_base,
                           std::uint64_t destination) {
    Bytes({0x48, 0x8d, 0x05});
    U32(0);
    const std::uint64_t captured = code_base + cursor_;
    MovAbsR10(destination);
    Bytes({0x49, 0x89, 0x02});
    return captured;
  }

private:
  std::uint8_t* code_{};
  std::size_t cursor_{};
};

bool ContainsFexThunkIdentity(std::uint64_t stub) {
  const auto* bytes = reinterpret_cast<const std::uint8_t*>(stub);
  const auto& identity = Lsx4::Translation::FexThunkIdentity;
  for (std::size_t offset = 0;
       offset + 2 + identity.size() <= Lsx4::FexHleLease::StubCodeBytes;
       ++offset) {
    if (bytes[offset] == 0x0f && bytes[offset + 1] == 0x3f &&
        std::memcmp(bytes + offset + 2, identity.data(), identity.size()) == 0) {
      return true;
    }
  }
  return false;
}

bool ContainsTokenLeaseCasProtocol(std::uint64_t stub) {
  const auto* bytes = reinterpret_cast<const std::uint8_t*>(stub);
  const std::size_t emitted = static_cast<std::size_t>(
      executor_lsx4_fex_hle_integration_stub_max_emitted_size());
  if (emitted == 0 || emitted > Lsx4::FexHleLease::StubCodeBytes) {
    return false;
  }
  std::size_t protocol_end = emitted;
  for (std::size_t offset = 0; offset + 1 < emitted; ++offset) {
    if (bytes[offset] == 0x0f && bytes[offset + 1] == 0x3f) {
      protocol_end = offset;
      break;
    }
  }
  if (protocol_end == emitted) {
    return false;
  }
  std::size_t acquire_cas = 0;
  std::size_t release_cas = 0;
  std::size_t bounded_attempt_loads = 0;
  std::size_t bounded_attempt_decrements = 0;
  for (std::size_t offset = 0; offset + 5 <= protocol_end; ++offset) {
    acquire_cas += bytes[offset] == 0xf0 && bytes[offset + 1] == 0x4d &&
                   bytes[offset + 2] == 0x0f && bytes[offset + 3] == 0xb1 &&
                   bytes[offset + 4] == 0x1a;
    release_cas += bytes[offset] == 0xf0 && bytes[offset + 1] == 0x4c &&
                   bytes[offset + 2] == 0x0f && bytes[offset + 3] == 0xb1 &&
                   bytes[offset + 4] == 0x02;
    bounded_attempt_loads +=
        bytes[offset] == 0xb9 && bytes[offset + 1] == 0x00 &&
        bytes[offset + 2] == 0x00 && bytes[offset + 3] == 0x01 &&
        bytes[offset + 4] == 0x00;
    bounded_attempt_decrements +=
        bytes[offset] == 0xff && bytes[offset + 1] == 0xc9 &&
        bytes[offset + 2] == 0x0f && bytes[offset + 3] == 0x85;
    if (bytes[offset] == 0xf0 &&
        (bytes[offset + 1] & 0xf0) == 0x40 &&
        bytes[offset + 2] == 0x0f && bytes[offset + 3] == 0xba) {
      return false;
    }
  }
  return acquire_cas >= 2 && release_cas == 1 &&
         bounded_attempt_loads >= 2 && bounded_attempt_decrements >= 2;
}

void DumpBytes(const char* label, std::uint64_t address, std::size_t size) {
  const auto* bytes = reinterpret_cast<const std::uint8_t*>(address);
  std::fprintf(stderr,
               "FEX_HLE_GUEST_STUB_ROUNDTRIP DUMP label=%s address=0x%" PRIx64
               " size=%zu\n",
               label, address, size);
  for (std::size_t offset = 0; offset < size; offset += 16) {
    std::fprintf(stderr, "  %04zx:", offset);
    for (std::size_t index = 0; index < 16 && offset + index < size; ++index) {
      std::fprintf(stderr, " %02x", bytes[offset + index]);
    }
    std::fputc('\n', stderr);
  }
}

GuestImage BuildGuestImage() {
  GuestImage image{};
  image.code = static_cast<std::uint8_t*>(
      Allocate(kPageSize, PROT_READ | PROT_WRITE | PROT_EXEC));
  image.stack = static_cast<std::uint8_t*>(
      Allocate(kStackSize, PROT_READ | PROT_WRITE));
  image.low_stack = static_cast<std::uint8_t*>(
      AllocateAt(UINT64_C(0x60000000), kStackSize));
  image.high7_stack = static_cast<std::uint8_t*>(
      AllocateAt(UINT64_C(0x700100000), kStackSize));
  image.tls = static_cast<std::uint8_t*>(
      Allocate(kPageSize, PROT_READ | PROT_WRITE));
  image.probe = static_cast<Probe*>(
      Allocate(kPageSize, PROT_READ | PROT_WRITE));
  Require(image.code != nullptr && image.stack != nullptr &&
              image.low_stack != nullptr && image.high7_stack != nullptr &&
              image.tls != nullptr && image.probe != nullptr,
          "guest-memory");
  *image.probe = {};
  executor_lsx4_android_register_guest_readable_range(
      image.stack, kStackSize, "fex-roundtrip-stack-pool");
  executor_lsx4_android_register_guest_readable_range(
      image.low_stack, kStackSize, "fex-roundtrip-stack-top32-zero");
  executor_lsx4_android_register_guest_readable_range(
      image.high7_stack, kStackSize, "fex-roundtrip-stack-top32-seven");
  executor_lsx4_android_register_guest_readable_range(
      image.probe, kPageSize, "fex-roundtrip-probe-sret");
  executor_lsx4_android_register_guest_readable_range(
      image.tls, kPageSize, "fex-roundtrip-tls");

  executor_lsx4_fex_hle_integration_begin_generation();
  const auto register_and_make_stub = []<typename Fn>(const char* name,
                                                       Fn function) {
    static constexpr auto descriptor =
        Core::AeroLib::HleAbiDetail::MakeDescriptor<Fn>();
    const auto native = reinterpret_cast<std::uint64_t>(function);
    Require(descriptor.supported && descriptor.descriptor_id != 0 &&
                descriptor.invoke != nullptr,
            "local-typed-descriptor", descriptor.descriptor_id);
    Require(executor_lsx4_fex_hle_integration_register(
                native, name, &descriptor) == LSX4_FEX_STATUS_OK,
            "production-register", native);
    return executor_lsx4_fex_hle_integration_make_stub(native, name);
  };
  image.integer_stub =
      register_and_make_stub("roundtrip-integer", &RoundtripInteger);
  image.fp_stub = register_and_make_stub("roundtrip-fp", &RoundtripFp);
  static constexpr auto stack_descriptor =
      Core::AeroLib::HleAbiDetail::MakeDescriptor<decltype(&RoundtripStackFp)>();
  Require(stack_descriptor.supported &&
              executor_lsx4_fex_hle_integration_register(
                  reinterpret_cast<std::uint64_t>(&RoundtripStackFp),
                  "roundtrip-stack-negative", &stack_descriptor) ==
                  LSX4_FEX_STATUS_OK,
          "production-register-stack-negative");
  const auto variadic_descriptor = VariadicDescriptor();
  const auto variadic_native =
      reinterpret_cast<std::uint64_t>(&RoundtripVariadicMarker);
  Require(executor_lsx4_fex_hle_integration_register(
              variadic_native, "roundtrip-variadic", &variadic_descriptor) ==
              LSX4_FEX_STATUS_OK,
          "production-register-variadic", variadic_native);
  image.variadic_stub = executor_lsx4_fex_hle_integration_make_stub(
      variadic_native, "roundtrip-variadic");
  image.sret_stub =
      register_and_make_stub("roundtrip-sret", &RoundtripSret);
  image.nested_driver_stub = register_and_make_stub(
      "roundtrip-nested-driver", &RunNestedGuestCallback);
  image.nested_queue_stub = register_and_make_stub(
      "roundtrip-nested-queue", &QueueNestedSignal);
  g_pool_wrap_probe_stub.store(image.nested_queue_stub,
                               std::memory_order_release);
  std::array<char, 512> generation_error{};
  Require(executor_lsx4_fex_hle_integration_seal_generation(
              generation_error.data(), generation_error.size()) ==
              LSX4_FEX_STATUS_OK,
          "typed-generation-seal");
  Require(executor_lsx4_fex_hle_integration_make_stub(0, "zero") == 0,
          "stub-zero-rejected");
  Require(image.integer_stub != 0 && image.fp_stub != 0 &&
              image.variadic_stub != 0 &&
              image.sret_stub != 0 && image.nested_driver_stub != 0 &&
              image.nested_queue_stub != 0 &&
              ContainsFexThunkIdentity(image.integer_stub) &&
              ContainsFexThunkIdentity(image.fp_stub) &&
              ContainsFexThunkIdentity(image.variadic_stub) &&
              ContainsFexThunkIdentity(image.sret_stub) &&
              ContainsFexThunkIdentity(image.nested_driver_stub) &&
              ContainsFexThunkIdentity(image.nested_queue_stub) &&
              ContainsTokenLeaseCasProtocol(image.integer_stub) &&
              executor_lsx4_fex_hle_integration_stub_canary_failures() == 0,
          "production-fex-stub-identity");

  std::memset(image.code, 0xcc, kPageSize);
  ImageBuilder out{image.code};
  const std::uint64_t code_base = reinterpret_cast<std::uint64_t>(image.code);
  const std::uint64_t probe = reinterpret_cast<std::uint64_t>(image.probe);
  g_nested_guest_function.store(code_base + kNestedCallbackOffset,
                                std::memory_order_release);

  // SysV AMD64 requires RSP to be 16-byte aligned immediately before CALL.
  // The mapped-entry sentinel makes function-entry RSP == 8 (mod 16), so use
  // the normal one-slot prologue before exercising production HLE stubs.
  out.Bytes({0x48, 0x83, 0xec, 0x08});
  out.StoreRsp(probe + offsetof(Probe, before_rsp));
  out.Bytes({0x48, 0xbf});
  out.U64(kIntegerFirst);
  out.Bytes({0x48, 0xbe});
  out.U64(kIntegerSecond);
  out.CallAbsolute(image.integer_stub);
  image.integer_call_return = code_base + out.cursor();
  out.StoreRax(probe + offsetof(Probe, integer_result));
  image.integer_continuation = out.CaptureRip(
      code_base, probe + offsetof(Probe, integer_continuation_rip));
  out.StoreRsp(probe + offsetof(Probe, after_integer_rsp));
  out.MovAbsRax(probe + offsetof(Probe, signal_handler_count));
  out.Bytes({0x48, 0x8b, 0x00});
  out.StoreRax(probe + offsetof(Probe, signal_seen_at_continuation));

  out.MovAbsRax(std::bit_cast<std::uint64_t>(kFpFirst));
  out.Bytes({0x66, 0x48, 0x0f, 0x6e, 0xc0});
  out.MovAbsRax(std::bit_cast<std::uint64_t>(kFpSecond));
  out.Bytes({0x66, 0x48, 0x0f, 0x6e, 0xc8});
  out.CallAbsolute(image.fp_stub);
  out.StoreXmm0(probe + offsetof(Probe, fp_result));
  image.fp_continuation = out.CaptureRip(
      code_base, probe + offsetof(Probe, fp_continuation_rip));
  out.StoreRsp(probe + offsetof(Probe, after_fp_rsp));

  out.MovAbsRax(kVariadicFirst);
  out.Bytes({0x66, 0x48, 0x0f, 0x6e, 0xc0});
  out.MovAbsRax(kVariadicSecond);
  out.Bytes({0x66, 0x48, 0x0f, 0x6e, 0xc8});
  out.Bytes({0xb8});
  out.U32(8);
  out.CallAbsolute(image.variadic_stub);
  out.StoreRax(probe + offsetof(Probe, variadic_result));
  image.variadic_continuation = out.CaptureRip(
      code_base, probe + offsetof(Probe, variadic_continuation_rip));
  out.StoreRsp(probe + offsetof(Probe, after_variadic_rsp));

  out.Bytes({0x48, 0xbf});
  out.U64(probe + offsetof(Probe, sret));
  out.Bytes({0x48, 0xbe});
  out.U64(kSretSeed);
  out.MovAbsRax(kSretMarker);
  out.Bytes({0x66, 0x48, 0x0f, 0x6e, 0xc0});
  out.CallAbsolute(image.sret_stub);
  out.StoreRax(probe + offsetof(Probe, sret_rax));
  image.sret_continuation = out.CaptureRip(
      code_base, probe + offsetof(Probe, sret_continuation_rip));
  out.StoreRsp(probe + offsetof(Probe, after_sret_rsp));

  out.Bytes({0xbf});
  out.U32(static_cast<std::uint32_t>(kExpectedExit));
  out.Bytes({0xb8});
  out.U32(static_cast<std::uint32_t>(kGuestExitSyscall));
  out.Bytes({0x0f, 0x05, 0xcc});

  out.Seek(kSignalRedirectOffset);
  out.MovAbsR10(probe + offsetof(Probe, signal_redirect_seen));
  out.Bytes({0x49, 0xc7, 0x02, 0x01, 0x00, 0x00, 0x00});
  out.Bytes({0xbf});
  out.U32(static_cast<std::uint32_t>(kSignalRedirectExit));
  out.Bytes({0xb8});
  out.U32(static_cast<std::uint32_t>(kGuestExitSyscall));
  out.Bytes({0x0f, 0x05, 0xcc});

  out.Seek(kSignalHandlerOffset);
  out.MovAbsR10(probe + offsetof(Probe, signal_handler_count));
  out.Bytes({0x49, 0xff, 0x02});
  out.Bytes({0x52});
  out.CallAbsolute(image.nested_driver_stub);
  out.Bytes({0x5a});
  out.StoreRax(probe + offsetof(Probe, nested_driver_result));
  out.Bytes({0x48, 0x8b, 0x42, 0x78});
  out.StoreRax(probe + offsetof(Probe, signal_context_rax));
  out.Bytes({0x48, 0x8b, 0x82, 0xe0, 0x00, 0x00, 0x00});
  out.StoreRax(probe + offsetof(Probe, signal_context_rip));
  out.Bytes({0x48, 0x8b, 0x82, 0xf8, 0x00, 0x00, 0x00});
  out.StoreRax(probe + offsetof(Probe, signal_context_rsp));
  out.MovAbsRax(code_base + kSignalRedirectOffset);
  out.Bytes({0x48, 0x89, 0x82, 0xe0, 0x00, 0x00, 0x00, 0xc3});

  out.Seek(kNestedSignalHandlerOffset);
  out.MovAbsR10(probe + offsetof(Probe, nested_signal_handler_count));
  out.Bytes({0x49, 0xff, 0x02, 0xc3});

  out.Seek(kNestedCallbackOffset);
  out.MovAbsR10(probe + offsetof(Probe, nested_callback_count));
  out.Bytes({0x49, 0xff, 0x02});
  out.Bytes({0x48, 0x83, 0xec, 0x08});
  out.CallAbsolute(image.nested_queue_stub);
  out.Bytes({0x48, 0x83, 0xc4, 0x08});
  out.StoreRax(probe + offsetof(Probe, nested_queue_result));
  out.MovAbsRax(probe + offsetof(Probe, nested_signal_handler_count));
  out.Bytes({0x48, 0x8b, 0x00});
  out.StoreRax(probe + offsetof(Probe, nested_signal_seen_inside_callback));
  out.Bytes({0xc3});
  __builtin___clear_cache(reinterpret_cast<char*>(image.code),
                          reinterpret_cast<char*>(image.code + kPageSize));

  const std::uint64_t slab =
      executor_lsx4_fex_hle_integration_stub_slab_base();
  const std::uint64_t slab_size =
      executor_lsx4_fex_hle_integration_stub_slab_size();
  Require(slab != 0 && slab_size != 0 && image.integer_stub >= slab &&
              image.integer_stub < slab + slab_size && image.fp_stub >= slab &&
              image.fp_stub < slab + slab_size && image.variadic_stub >= slab &&
              image.variadic_stub < slab + slab_size && image.sret_stub >= slab &&
              image.sret_stub < slab + slab_size &&
              image.nested_driver_stub >= slab &&
              image.nested_driver_stub < slab + slab_size &&
              image.nested_queue_stub >= slab &&
              image.nested_queue_stub < slab + slab_size,
          "stub-slab-membership");
  DumpBytes("integer-production-stub", image.integer_stub,
            Lsx4::FexHleLease::StubCodeBytes);
  DumpBytes("guest-code", code_base, 256);
  std::fprintf(stderr,
               "FEX_HLE_GUEST_STUB_ROUNDTRIP ADDRESSES code=0x%" PRIx64
               " stack=0x%" PRIx64 " low=0x%" PRIx64 " high7=0x%" PRIx64
               " probe=0x%" PRIx64 " slab=0x%" PRIx64 " slab_size=0x%" PRIx64
               "\n",
               code_base, reinterpret_cast<std::uint64_t>(image.stack),
               reinterpret_cast<std::uint64_t>(image.low_stack),
               reinterpret_cast<std::uint64_t>(image.high7_stack), probe, slab,
               slab_size);
  image.ranges[0] = {
      .address = code_base,
      .size = kPageSize,
      .range_cookie = UINT64_C(0x524f554e44434f44),
      .protection = PROT_READ | PROT_WRITE | PROT_EXEC,
      .range_flags = LSX4_FEX_RANGE_EXECUTABLE | LSX4_FEX_RANGE_WRITABLE |
                     LSX4_FEX_RANGE_SELF_MODIFYING,
  };
  image.ranges[1] = {
      .address = slab,
      .size = slab_size,
      .range_cookie = UINT64_C(0x524f554e44535442),
      .protection = PROT_READ | PROT_EXEC,
      .range_flags = LSX4_FEX_RANGE_EXECUTABLE |
                     LSX4_FEX_RANGE_HLE_TRAMPOLINE,
  };
  image.range_snapshot = {
      .struct_size = sizeof(Lsx4FexExecutableRangeSnapshotV5),
      .abi_version = LSX4_FEX_EXECUTABLE_RANGE_ABI_V5,
      .generation = Lsx4::FexHleLease::InitialManifestGeneration,
      .ranges = image.ranges.data(),
      .capacity = static_cast<std::uint32_t>(image.ranges.size()),
      .range_count = static_cast<std::uint32_t>(image.ranges.size()),
      .snapshot_flags = 0,
      .reserved = 0,
  };
  return image;
}

void LogCallback(void*, std::int32_t priority, const char* component,
                 const char* message) {
  std::fprintf(stderr, "FEX_HLE_GUEST_STUB_ROUNDTRIP LOG p=%d c=%s m=%s\n",
               priority, component != nullptr ? component : "-",
               message != nullptr ? message : "-");
}

std::int32_t ProductionDispatch(void*, const Lsx4FexHleCallFrameV3* call,
                                 Lsx4FexHleResultFrameV3* result) {
  if (call != nullptr &&
      call->native_function ==
          reinterpret_cast<std::uint64_t>(&RoundtripInteger)) {
    g_integer_guest_rip.store(call->guest_rip, std::memory_order_relaxed);
  }
  std::fprintf(stderr,
               "FEX_HLE_GUEST_STUB_ROUNDTRIP DISPATCH_ENTRY native=0x%" PRIx64
               " rip=0x%" PRIx64 " rsp=0x%" PRIx64 " input_rax=0x%" PRIx64
               " call=%" PRIu64 "\n",
               call != nullptr ? call->native_function : 0,
               call != nullptr ? call->guest_rip : 0,
               call != nullptr ? call->guest_rsp : 0,
               call != nullptr ? call->input_rax : 0,
               call != nullptr ? call->call_id : 0);
  const std::int32_t status =
      executor_lsx4_fex_hle_integration_dispatch(call, result);
  std::fprintf(stderr,
               "FEX_HLE_GUEST_STUB_ROUNDTRIP DISPATCH native=0x%" PRIx64
               " rip=0x%" PRIx64 " rsp=0x%" PRIx64 " input_rax=0x%" PRIx64
               " status=%d result=%d call=%" PRIu64 "/%" PRIu64 "\n",
               call != nullptr ? call->native_function : 0,
               call != nullptr ? call->guest_rip : 0,
               call != nullptr ? call->guest_rsp : 0,
               call != nullptr ? call->input_rax : 0, status,
               result != nullptr ? result->status : 0,
               call != nullptr ? call->call_id : 0,
               result != nullptr ? result->call_id : 0);
  return status;
}

void ThreadEvent(void*, std::uint32_t,
                 const Lsx4FexThreadIdentityV3*) {}

void FatalCallback(void*, std::int32_t status, const char* message) {
  g_fatal_calls.fetch_add(1, std::memory_order_relaxed);
  std::fprintf(stderr,
               "FEX_HLE_GUEST_STUB_ROUNDTRIP FATAL status=%d message=%s\n",
               status, message != nullptr ? message : "-");
}

void AppendSignalText(char* buffer, std::size_t capacity, std::size_t& cursor,
                      const char* text) {
  while (*text != '\0' && cursor < capacity) {
    buffer[cursor++] = *text++;
  }
}

void AppendSignalHex(char* buffer, std::size_t capacity, std::size_t& cursor,
                     std::uint64_t value) {
  constexpr char kHex[] = "0123456789abcdef";
  AppendSignalText(buffer, capacity, cursor, "0x");
  bool started = false;
  for (int shift = 60; shift >= 0; shift -= 4) {
    const unsigned digit = static_cast<unsigned>((value >> shift) & 0xf);
    if (digit != 0 || started || shift == 0) {
      started = true;
      if (cursor < capacity) {
        buffer[cursor++] = kHex[digit];
      }
    }
  }
}

int SignalHexDigit(char value) {
  if (value >= '0' && value <= '9') {
    return value - '0';
  }
  if (value >= 'a' && value <= 'f') {
    return value - 'a' + 10;
  }
  if (value >= 'A' && value <= 'F') {
    return value - 'A' + 10;
  }
  return -1;
}

void DumpSignalMapForPc(std::uint64_t pc) {
  const int maps = open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
  if (maps < 0) {
    return;
  }
  char input[4096];
  char line[1024];
  std::size_t line_size = 0;
  ssize_t count = 0;
  bool done = false;
  while (!done && (count = read(maps, input, sizeof(input))) > 0) {
    for (ssize_t index = 0; index < count; ++index) {
      const char ch = input[index];
      if (ch != '\n' && line_size + 1 < sizeof(line)) {
        line[line_size++] = ch;
        continue;
      }
      std::uint64_t first = 0;
      std::uint64_t last = 0;
      std::size_t cursor = 0;
      int digit = 0;
      while (cursor < line_size && (digit = SignalHexDigit(line[cursor])) >= 0) {
        first = (first << 4) | static_cast<unsigned>(digit);
        ++cursor;
      }
      if (cursor < line_size && line[cursor++] == '-') {
        while (cursor < line_size &&
               (digit = SignalHexDigit(line[cursor])) >= 0) {
          last = (last << 4) | static_cast<unsigned>(digit);
          ++cursor;
        }
      }
      if (pc >= first && pc < last) {
        constexpr char prefix[] =
            "FEX_HLE_GUEST_STUB_ROUNDTRIP PC_MAP ";
        (void)write(STDERR_FILENO, prefix, sizeof(prefix) - 1);
        (void)write(STDERR_FILENO, line, line_size);
        (void)write(STDERR_FILENO, "\n", 1);
        done = true;
        break;
      }
      line_size = 0;
    }
  }
  close(maps);
}

void HostSignalHandler(int signum, siginfo_t* info, void* raw_context) {
#if defined(__aarch64__)
  const auto* initial_context = static_cast<const ucontext_t*>(raw_context);
  const std::uint64_t initial_pc =
      initial_context != nullptr ? initial_context->uc_mcontext.pc : 0;
#else
  const std::uint64_t initial_pc = 0;
#endif
  if (signum == SIGURG) {
    g_host_sigurg_seen = 1;
  }
  {
    char captured[512]{};
    std::size_t captured_length = 0;
    AppendSignalText(captured, sizeof(captured), captured_length,
                     "FEX_HLE_GUEST_STUB_ROUNDTRIP captured signal=");
    AppendSignalHex(captured, sizeof(captured), captured_length,
                    static_cast<std::uint64_t>(signum));
    AppendSignalText(captured, sizeof(captured), captured_length, " addr=");
    AppendSignalHex(captured, sizeof(captured), captured_length,
                    reinterpret_cast<std::uint64_t>(
                        info != nullptr ? info->si_addr : nullptr));
    AppendSignalText(captured, sizeof(captured), captured_length, " pc=");
    AppendSignalHex(captured, sizeof(captured), captured_length, initial_pc);
#if defined(__aarch64__)
    const auto* captured_context = static_cast<const ucontext_t*>(raw_context);
    if (captured_context != nullptr) {
      for (const int reg : {0, 1, 2, 19, 20, 28}) {
        AppendSignalText(captured, sizeof(captured), captured_length, " x");
        AppendSignalHex(captured, sizeof(captured), captured_length,
                        static_cast<std::uint64_t>(reg));
        AppendSignalText(captured, sizeof(captured), captured_length, "=");
        AppendSignalHex(captured, sizeof(captured), captured_length,
                        captured_context->uc_mcontext.regs[reg]);
      }
      AppendSignalText(captured, sizeof(captured), captured_length, " sp=");
      AppendSignalHex(captured, sizeof(captured), captured_length,
                      captured_context->uc_mcontext.sp);
      if (initial_pc != 0) {
        std::uint32_t instruction{};
        std::memcpy(&instruction, reinterpret_cast<const void*>(initial_pc),
                    sizeof(instruction));
        AppendSignalText(captured, sizeof(captured), captured_length, " insn=");
        AppendSignalHex(captured, sizeof(captured), captured_length,
                        instruction);
      }
    }
#endif
    AppendSignalText(captured, sizeof(captured), captured_length, "\n");
    (void)write(STDERR_FILENO, captured, captured_length);
  }
  const Lsx4FexBackendApiV3* api = g_api;
  const std::int32_t disposition =
      api != nullptr && api->handle_host_signal != nullptr
          ? api->handle_host_signal(api->backend_context, signum, info,
                                    raw_context)
          : LSX4_FEX_HOST_SIGNAL_NOT_HANDLED;
  if (disposition == LSX4_FEX_HOST_SIGNAL_HANDLED || signum == SIGURG) {
    return;
  }
  char message[256]{};
  std::size_t length = 0;
  AppendSignalText(message, sizeof(message), length,
                   "FEX_HLE_GUEST_STUB_ROUNDTRIP unhandled ");
  if (signum == SIGSEGV) {
    AppendSignalText(message, sizeof(message), length, "SIGSEGV");
  } else if (signum == SIGBUS) {
    AppendSignalText(message, sizeof(message), length, "SIGBUS");
  } else if (signum == SIGILL) {
    AppendSignalText(message, sizeof(message), length, "SIGILL");
  } else if (signum == SIGTRAP) {
    AppendSignalText(message, sizeof(message), length, "SIGTRAP");
  } else {
    AppendSignalText(message, sizeof(message), length, "signal");
  }
  AppendSignalText(message, sizeof(message), length, " addr=");
  AppendSignalHex(message, sizeof(message), length,
                  reinterpret_cast<std::uint64_t>(info != nullptr
                                                      ? info->si_addr
                                                      : nullptr));
#if defined(__aarch64__)
  const auto* context = static_cast<const ucontext_t*>(raw_context);
  AppendSignalText(message, sizeof(message), length, " pc_before=");
  AppendSignalHex(message, sizeof(message), length, initial_pc);
  AppendSignalText(message, sizeof(message), length, " pc_after=");
  AppendSignalHex(message, sizeof(message), length,
                  context != nullptr ? context->uc_mcontext.pc : 0);
#endif
  AppendSignalText(message, sizeof(message), length, "\n");
  (void)write(STDERR_FILENO, message, length);
  DumpSignalMapForPc(initial_pc);
  _exit(128 + signum);
}

void InstallSignalOwner() {
  constexpr std::size_t kAltStackSize = 64 * 1024;
  void* memory = Allocate(kAltStackSize, PROT_READ | PROT_WRITE);
  Require(memory != nullptr, "altstack-memory");
  stack_t stack{};
  stack.ss_sp = memory;
  stack.ss_size = kAltStackSize;
  Require(sigaltstack(&stack, nullptr) == 0, "sigaltstack");
  struct sigaction action {};
  action.sa_sigaction = HostSignalHandler;
  sigemptyset(&action.sa_mask);
  action.sa_flags = SA_SIGINFO | SA_ONSTACK;
  for (const int signal : {SIGSEGV, SIGBUS, SIGILL, SIGTRAP, SIGURG}) {
    Require(sigaction(signal, &action, nullptr) == 0, "sigaction", signal);
  }
}

void* QueueSignalThread(void*) {
  while (g_hle_entered.load(std::memory_order_acquire) == 0) {
    (void)syscall(SYS_sched_yield);
  }
  Lsx4FexGuestSignalDeliveryV3 delivery{
      .struct_size = sizeof(Lsx4FexGuestSignalDeliveryV3),
      .abi_version = LSX4_FEX_BACKEND_ABI_V3,
      .target = g_main_identity,
      .signum = kQueuedSignal,
      .delivery_flags = 0,
      .explicit_rip = 0,
      .explicit_rsp = 0,
      .explicit_context = nullptr,
  };
  const int status = g_api->queue_guest_signal(g_api->backend_context, &delivery);
  g_signal_queue_status.store(status, std::memory_order_release);
  g_signal_queue_done.store(1, std::memory_order_release);
  return nullptr;
}

Lsx4FexThreadIdentityV3 Identity() {
  return {
      .struct_size = sizeof(Lsx4FexThreadIdentityV3),
      .abi_version = LSX4_FEX_BACKEND_ABI_V3,
      .guest_thread = kGuestThread,
      .pthread_handle = 0,
      .host_tid = 0,
      .backend_thread = 0,
  };
}

Lsx4FexHleResultFrameV3 EmptyResult(std::uint64_t call_id) {
  return {
      .struct_size = sizeof(Lsx4FexHleResultFrameV3),
      .abi_version = LSX4_FEX_BACKEND_ABI_V3,
      .call_id = call_id,
  };
}

void TestUnregisteredInaccessibleGuestMemoryFailsClosed() {
  void* inaccessible = Allocate(kPageSize, PROT_NONE);
  Require(inaccessible != nullptr, "unregistered-inaccessible-allocation");

  const std::uint64_t address =
      reinterpret_cast<std::uint64_t>(inaccessible);
  std::uint64_t scratch = UINT64_C(0x554e524547495354);
  Require(!ExecutorJitReadGuestBytes(address, &scratch, sizeof(scratch)),
          "unregistered-inaccessible-read-fail-closed");
  Require(!ExecutorJitWriteGuestBytes(address, &scratch, sizeof(scratch)),
          "unregistered-inaccessible-write-fail-closed");
  Require(munmap(inaccessible, kPageSize) == 0,
          "unregistered-inaccessible-release");
}

void TestFailClosedMemoryAndVariadicGates(const GuestImage& image) {
  auto* guarded = static_cast<std::uint8_t*>(
      Allocate(kPageSize * 2, PROT_READ | PROT_WRITE));
  Require(guarded != nullptr, "negative-guard-allocation");
  executor_lsx4_android_register_guest_readable_range(
      guarded, kPageSize * 2, "fex-roundtrip-stale-registered-guard");
  Require(mprotect(guarded + kPageSize, kPageSize, PROT_NONE) == 0,
          "negative-guard-page");
  const std::uint64_t denied =
      reinterpret_cast<std::uint64_t>(guarded + kPageSize);
  const std::uint64_t valid_rsp =
      reinterpret_cast<std::uint64_t>(image.stack) + 0x100;

  Lsx4FexHleCallFrameV3 stack_call{
      .struct_size = sizeof(Lsx4FexHleCallFrameV3),
      .abi_version = LSX4_FEX_BACKEND_ABI_V3,
      .call_id = 0x1001,
      .native_function = reinterpret_cast<std::uint64_t>(&RoundtripStackFp),
      .guest_rip = reinterpret_cast<std::uint64_t>(image.code),
      .guest_rsp = denied - sizeof(std::uint64_t),
  };
  for (std::size_t index = 0; index < 8; ++index) {
    stack_call.xmm[index].low =
        std::bit_cast<std::uint64_t>(static_cast<double>(index + 1));
  }
  auto stack_result = EmptyResult(stack_call.call_id);
  Require(ProductionDispatch(nullptr, &stack_call, &stack_result) ==
                  LSX4_FEX_STATUS_BACKEND_FAILURE &&
              stack_result.status == LSX4_FEX_STATUS_BACKEND_FAILURE,
          "unmapped-guest-stack-fail-closed", stack_result.status,
          LSX4_FEX_STATUS_BACKEND_FAILURE);

  Lsx4FexHleCallFrameV3 sret_call{
      .struct_size = sizeof(Lsx4FexHleCallFrameV3),
      .abi_version = LSX4_FEX_BACKEND_ABI_V3,
      .call_id = 0x1002,
      .native_function = reinterpret_cast<std::uint64_t>(&RoundtripSret),
      .guest_rip = reinterpret_cast<std::uint64_t>(image.code),
      .guest_rsp = valid_rsp,
  };
  sret_call.gpr[0] = denied;
  sret_call.gpr[1] = kSretSeed;
  sret_call.xmm[0].low = kSretMarker;
  auto sret_result = EmptyResult(sret_call.call_id);
  Require(ProductionDispatch(nullptr, &sret_call, &sret_result) ==
                  LSX4_FEX_STATUS_BACKEND_FAILURE &&
              sret_result.status == LSX4_FEX_STATUS_BACKEND_FAILURE,
          "unmapped-sret-fail-closed", sret_result.status,
          LSX4_FEX_STATUS_BACKEND_FAILURE);

  Lsx4FexHleCallFrameV3 variadic_call{
      .struct_size = sizeof(Lsx4FexHleCallFrameV3),
      .abi_version = LSX4_FEX_BACKEND_ABI_V3,
      .call_id = 0x1003,
      .native_function =
          reinterpret_cast<std::uint64_t>(&RoundtripVariadicMarker),
      .guest_rip = reinterpret_cast<std::uint64_t>(image.code),
      .guest_rsp = valid_rsp,
      .input_rax = 9,
  };
  variadic_call.xmm[0].low = kVariadicFirst;
  variadic_call.xmm[1].low = kVariadicSecond;
  auto variadic_result = EmptyResult(variadic_call.call_id);
  Require(ProductionDispatch(nullptr, &variadic_call, &variadic_result) ==
                  LSX4_FEX_STATUS_INVALID_ARGUMENT &&
              variadic_result.status == LSX4_FEX_STATUS_INVALID_ARGUMENT,
          "variadic-al-nine-rejected", variadic_result.status,
          LSX4_FEX_STATUS_INVALID_ARGUMENT);
}

} // namespace

int main(int argc, char** argv) {
  Require(argc == 2 || argc == 3, "usage-backend-path", argc, 2);
  g_plain_hle_mode = argc == 3 && std::strcmp(argv[2], "--plain-hle") == 0;
  std::fprintf(stderr, "FEX_HLE_GUEST_STUB_ROUNDTRIP STAGE build-image\n");
  GuestImage image = BuildGuestImage();
  std::fprintf(stderr, "FEX_HLE_GUEST_STUB_ROUNDTRIP STAGE install-signals\n");
  InstallSignalOwner();
  std::fprintf(stderr, "FEX_HLE_GUEST_STUB_ROUNDTRIP STAGE load-backend\n");

  void* handle = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
  if (handle == nullptr) {
    std::fprintf(stderr, "FEX_HLE_GUEST_STUB_ROUNDTRIP dlerror=%s\n", dlerror());
    Fail("dlopen-backend");
  }
  std::fprintf(stderr, "FEX_HLE_GUEST_STUB_ROUNDTRIP STAGE backend-loaded\n");
  auto get_api = reinterpret_cast<Lsx4FexGetBackendApiV3Fn>(
      dlsym(handle, LSX4_FEX_BACKEND_GET_API_V3_SYMBOL));
  Require(get_api != nullptr, "dlsym-backend-api");
  Lsx4FexBackendApiV3 api{};
  Require(get_api(LSX4_FEX_BACKEND_ABI_V3, sizeof(api), &api) ==
                  LSX4_FEX_STATUS_OK &&
              (api.provided_capabilities & kRequiredCapabilities) ==
                  kRequiredCapabilities,
          "backend-api");
  std::fprintf(stderr, "FEX_HLE_GUEST_STUB_ROUNDTRIP STAGE backend-api\n");
  g_api = &api;

  const std::string runtime_root = DirectoryOf(argv[1]);
  const Lsx4FexGuestCpuProfileV6 guest_cpu_profile{
      .struct_size = sizeof(Lsx4FexGuestCpuProfileV6),
      .abi_version = LSX4_FEX_GUEST_CPU_PROFILE_ABI_V6,
      .identity_low = UINT64_C(0x61e82729ec820a6c),
      .identity_high = UINT64_C(0xf0e351bb9bbb26d1),
      .profile_id = "lsx4-ps4-hle-roundtrip-v1",
      .profile_context = nullptr,
      .query_cpuid = QueryPs4Cpuid,
      .query_xcr = QueryPs4Xcr,
      .xcr0_writable_mask = 0,
      .mxcsr_reset_value = UINT32_C(0x00001f80),
      .mxcsr_mask = UINT32_C(0x0000ffff),
      .legacy_save_area_bytes = 512,
      .profile_flags = 0,
  };
  Lsx4FexConfigureRequestV3 configure{
      .struct_size = sizeof(Lsx4FexConfigureRequestV3),
      .abi_version = LSX4_FEX_BACKEND_ABI_V3,
      .platform_mask = LSX4_FEX_PLATFORM_PS4,
      .configure_flags = 0,
      .runtime_root = runtime_root.c_str(),
      .cache_namespace = "lsx4-fex-hle-guest-stub-roundtrip-v1",
      .hle_thunk_identity = Lsx4::Translation::FexThunkIdentity.data(),
      .hle_thunk_identity_size = static_cast<std::uint32_t>(
          Lsx4::Translation::FexThunkIdentity.size()),
      .reserved = 0,
      .required_capabilities = kRequiredCapabilities,
      .host_build_id = "lsx4-fex-hle-guest-stub-roundtrip-v1",
      .request_cookie = UINT64_C(0x4753545542434647),
      .host_hwcap = 0,
      .host_hwcap2 = 0,
      .host_isa_features = 0,
      .host_os_features = 0,
      .sve_vector_bytes = 0,
      .reserved2 = 0,
      .guest_cpu_profile_v6 = &guest_cpu_profile,
  };
  Lsx4FexConfigureResultV3 configure_result{
      .struct_size = sizeof(Lsx4FexConfigureResultV3),
      .abi_version = LSX4_FEX_BACKEND_ABI_V3,
  };
  Require(api.configure(api.backend_context, &configure, &configure_result) ==
                  LSX4_FEX_STATUS_OK &&
              configure_result.status == LSX4_FEX_STATUS_OK,
          "configure", configure_result.status, LSX4_FEX_STATUS_OK);
  std::fprintf(stderr, "FEX_HLE_GUEST_STUB_ROUNDTRIP STAGE configured\n");

  const Lsx4FexHostCallbacksV3 callbacks{
      .struct_size = sizeof(Lsx4FexHostCallbacksV3),
      .abi_version = LSX4_FEX_BACKEND_ABI_V3,
      .callback_context = nullptr,
      .log = LogCallback,
      .dispatch_hle = ProductionDispatch,
      .thread_event = ThreadEvent,
      .fatal_error = FatalCallback,
      .acquire_guest_memory_lease = AcquireStableSmokeGuestMemoryLease,
      .release_guest_memory_lease = ReleaseStableSmokeGuestMemoryLease,
  };
  Lsx4FexActivateRequestV3 activate{
      .struct_size = sizeof(Lsx4FexActivateRequestV3),
      .abi_version = LSX4_FEX_BACKEND_ABI_V3,
      .activate_flags = 0,
      .reserved = 0,
      .host_callbacks = &callbacks,
      .initial_executable_ranges = &image.range_snapshot,
      .hle_thunk_identity = Lsx4::Translation::FexThunkIdentity.data(),
      .hle_thunk_identity_size = static_cast<std::uint32_t>(
          Lsx4::Translation::FexThunkIdentity.size()),
      .reserved2 = 0,
      .request_cookie = UINT64_C(0x4753545542414354),
  };
  Lsx4FexActivateResultV3 activate_result{
      .struct_size = sizeof(Lsx4FexActivateResultV3),
      .abi_version = LSX4_FEX_BACKEND_ABI_V3,
  };
  const int activate_status =
      api.activate(api.backend_context, &activate, &activate_result);
  if (activate_status != LSX4_FEX_STATUS_OK) {
    std::fprintf(stderr, "FEX_HLE_GUEST_STUB_ROUNDTRIP activate_error=%s\n",
                 activate_result.error_message != nullptr
                     ? activate_result.error_message
                     : "-");
  }
  Require(activate_status == LSX4_FEX_STATUS_OK &&
              activate_result.status == LSX4_FEX_STATUS_OK,
          "activate", activate_status, LSX4_FEX_STATUS_OK);
  std::fprintf(stderr, "FEX_HLE_GUEST_STUB_ROUNDTRIP STAGE activated\n");
  TestUnregisteredInaccessibleGuestMemoryFailsClosed();

  g_main_identity = Identity();
  Require(api.bind_host_thread(api.backend_context, &g_main_identity) ==
              LSX4_FEX_STATUS_OK,
          "bind-main");
  std::fprintf(stderr, "FEX_HLE_GUEST_STUB_ROUNDTRIP STAGE bound\n");
  Lsx4FexGuestTlsStateV3 tls{
      .struct_size = sizeof(Lsx4FexGuestTlsStateV3),
      .abi_version = LSX4_FEX_BACKEND_ABI_V3,
      .target = g_main_identity,
      .fs_base = reinterpret_cast<std::uint64_t>(image.tls) + 0x100,
      .gs_base = reinterpret_cast<std::uint64_t>(image.tls) + 0x180,
      .tcb_base = reinterpret_cast<std::uint64_t>(image.tls) + 0x100,
      .tls_image_address = reinterpret_cast<std::uint64_t>(image.tls),
      .tls_image_size = kPageSize,
      .tls_module_id = 1,
      .tls_flags = 0,
      .reserved = 0,
  };
  Require(api.set_guest_tls(api.backend_context, &tls) == LSX4_FEX_STATUS_OK,
          "set-tls");
  const Lsx4FexGuestSignalActionV3 signal_action{
      .struct_size = sizeof(Lsx4FexGuestSignalActionV3),
      .abi_version = LSX4_FEX_BACKEND_ABI_V3,
      .signum = kQueuedSignal,
      .action_flags = kOrbisSaSigInfo,
      .handler = reinterpret_cast<std::uint64_t>(image.code) +
                 kSignalHandlerOffset,
      .restorer = 0,
      .mask_low = 0,
      .mask_high = 0,
  };
  Require(api.register_guest_signal_action(api.backend_context,
                                            &signal_action) ==
              LSX4_FEX_STATUS_OK,
          "register-signal-action");
  const Lsx4FexGuestSignalActionV3 nested_signal_action{
      .struct_size = sizeof(Lsx4FexGuestSignalActionV3),
      .abi_version = LSX4_FEX_BACKEND_ABI_V3,
      .signum = kNestedQueuedSignal,
      .action_flags = 0,
      .handler = reinterpret_cast<std::uint64_t>(image.code) +
                 kNestedSignalHandlerOffset,
      .restorer = 0,
      .mask_low = 0,
      .mask_high = 0,
  };
  Require(api.register_guest_signal_action(api.backend_context,
                                            &nested_signal_action) ==
              LSX4_FEX_STATUS_OK,
          "register-nested-signal-action");
  std::fprintf(stderr, "FEX_HLE_GUEST_STUB_ROUNDTRIP STAGE signal-ready\n");

  pthread_t signal_thread{};
  if (!g_plain_hle_mode) {
    Require(pthread_create(&signal_thread, nullptr, QueueSignalThread, nullptr) ==
                0,
            "signal-thread-create");
  }

  std::array<Lsx4FexMappedSegmentV5, 2> segments{{
      {
          .address = reinterpret_cast<std::uint64_t>(image.code),
          .size = kPageSize,
          .range_cookie = image.ranges[0].range_cookie,
          .protection = PROT_READ | PROT_WRITE | PROT_EXEC,
          .range_flags = image.ranges[0].range_flags,
      },
      {
          .address = reinterpret_cast<std::uint64_t>(image.stack),
          .size = kStackSize,
          .range_cookie = UINT64_C(0x475354554253544b),
          .protection = PROT_READ | PROT_WRITE,
          .range_flags = 0,
      },
  }};
  Lsx4FexMappedEntryRequestV5 request{
      .struct_size = sizeof(Lsx4FexMappedEntryRequestV5),
      .abi_version = LSX4_FEX_MAPPED_ENTRY_ABI_V5,
      .request_flags = 0,
      .timeout_ms = 0,
      .entry = reinterpret_cast<std::uint64_t>(image.code),
      .proc_param = 0,
      .stack_base = reinterpret_cast<std::uint64_t>(image.stack),
      .stack_size = kStackSize,
      .tls_image_address = reinterpret_cast<std::uint64_t>(image.tls),
      .tls_image_size = kPageSize,
      .tls_module_id = 1,
      .tcb_base = reinterpret_cast<std::uint64_t>(image.tls) + 0x100,
      .entry_params = 0,
      .exit_function = 0,
      .hle_trampoline_base =
          executor_lsx4_fex_hle_integration_stub_slab_base(),
      .hle_trampoline_size =
          executor_lsx4_fex_hle_integration_stub_slab_size(),
      .segments = segments.data(),
      .segment_count = static_cast<std::uint32_t>(segments.size()),
      .reserved = 0,
      .executable_ranges = &image.range_snapshot,
      .request_cookie = UINT64_C(0x475354554252554e),
  };
  Lsx4FexMappedEntryResultV5 result{
      .struct_size = sizeof(Lsx4FexMappedEntryResultV5),
      .abi_version = LSX4_FEX_MAPPED_ENTRY_ABI_V5,
  };
  const int run_status =
      api.run_mapped_entry_v5(api.backend_context, &request, &result);
  std::fprintf(stderr, "FEX_HLE_GUEST_STUB_ROUNDTRIP STAGE first-run-returned status=%d result=%d\n",
               run_status, result.status);
  if (g_plain_hle_mode) {
    Require(run_status == LSX4_FEX_STATUS_OK &&
                result.status == LSX4_FEX_STATUS_OK &&
                result.guest_exit_code == kExpectedExit,
            "plain-hle-exit", result.guest_exit_code, kExpectedExit);
    std::printf("FEX_HLE_GUEST_STUB_ROUNDTRIP PLAIN_PASS integer=1 fp=1 "
                "variadic=1 sret=1\n");
    std::fflush(stdout);
    std::_Exit(0);
  }
  Require(pthread_join(signal_thread, nullptr) == 0, "signal-thread-join");
  Require(run_status == LSX4_FEX_STATUS_OK &&
              result.status == LSX4_FEX_STATUS_OK &&
              result.guest_exit_code == kSignalRedirectExit,
          "signal-ucontext-rip-redirect", result.guest_exit_code,
          kSignalRedirectExit);
  const std::uint64_t pool_path_high = image.probe->before_rsp >> 32;
  Require(pool_path_high != 0 && pool_path_high != 7,
          "stub-pool-path-selected", pool_path_high);
  Require(g_signal_queue_status.load(std::memory_order_acquire) ==
                  LSX4_FEX_STATUS_OK &&
              g_host_sigurg_seen == 1,
          "queued-signal-host-return",
          g_signal_queue_status.load(std::memory_order_relaxed),
          LSX4_FEX_STATUS_OK);
  Require(image.probe->signal_handler_count == 1 &&
              image.probe->signal_redirect_seen == 1,
          "queued-signal-ucontext-rip-honored",
          image.probe->signal_redirect_seen, 1);
  Require(image.probe->signal_seen_at_continuation == 0 &&
              image.probe->signal_context_rip !=
                  reinterpret_cast<std::uint64_t>(image.code) +
                      kSignalRedirectOffset,
          "queued-signal-redirect-preempted-normal-continuation",
          image.probe->signal_seen_at_continuation, 0);
  Require(image.probe->nested_driver_result == kNestedDriverMagic &&
              image.probe->nested_callback_count == 1 &&
              g_nested_driver_calls.load(std::memory_order_acquire) == 1 &&
              g_nested_callback_api_status.load(std::memory_order_acquire) ==
                  LSX4_FEX_STATUS_OK &&
              g_nested_callback_result_status.load(
                  std::memory_order_acquire) == LSX4_FEX_STATUS_OK,
          "nested-signal-handler-hle-callback",
          image.probe->nested_driver_result, kNestedDriverMagic);
  Require(image.probe->nested_queue_result == LSX4_FEX_STATUS_OK &&
              g_nested_queue_calls.load(std::memory_order_acquire) == 1 &&
              g_nested_queue_status.load(std::memory_order_acquire) ==
                  LSX4_FEX_STATUS_OK,
          "nested-signal-queued-inside-callback",
          image.probe->nested_queue_result, LSX4_FEX_STATUS_OK);
  Require(image.probe->nested_signal_seen_inside_callback == 0 &&
              image.probe->nested_signal_handler_count == 1 &&
              g_pool_wrap_forced.load(std::memory_order_acquire) == 1,
          "nested-signal-delivered-by-outer-drain",
          image.probe->nested_signal_handler_count, 1);
  Require(g_integer_guest_rip.load(std::memory_order_relaxed) ==
              image.integer_call_return,
          "hle-call-guest-rip-continuation",
          g_integer_guest_rip.load(std::memory_order_relaxed),
          image.integer_call_return);

  const auto rerun_on_stack = [&](std::uint8_t* guest_stack,
                                  std::uint64_t cookie,
                                  std::uint64_t expected_high) {
    ArmHleScratchUnderflowCanary(guest_stack);
    segments[1].address = reinterpret_cast<std::uint64_t>(guest_stack);
    segments[1].range_cookie = cookie ^ UINT64_C(0x535441434b);
    request.stack_base = reinterpret_cast<std::uint64_t>(guest_stack);
    request.request_cookie = cookie;
    result = {
        .struct_size = sizeof(Lsx4FexMappedEntryResultV5),
        .abi_version = LSX4_FEX_MAPPED_ENTRY_ABI_V5,
    };
    const int status =
        api.run_mapped_entry_v5(api.backend_context, &request, &result);
    Require(status == LSX4_FEX_STATUS_OK &&
                result.status == LSX4_FEX_STATUS_OK &&
                result.guest_exit_code == kExpectedExit,
            "mapped-stack-path-rerun", result.guest_exit_code,
            kExpectedExit);
    Require((image.probe->before_rsp >> 32) == expected_high,
            "stub-stack-path-selected", image.probe->before_rsp >> 32,
            expected_high);
    Require((image.probe->before_rsp & 15) == 0,
            "sysv-caller-rsp-alignment", image.probe->before_rsp & 15, 0);
    RequireHleScratchUnderflowCanary(guest_stack,
                                    "hle-stack-scratch-underflow-canary");
  };
  rerun_on_stack(image.low_stack, UINT64_C(0x4c4f57535441434b), 0);
  rerun_on_stack(image.high7_stack, UINT64_C(0x484947483753544b), 7);

  Require(image.probe->integer_result == kIntegerExpected,
          "integer-result", image.probe->integer_result, kIntegerExpected);
  Require(image.probe->fp_result ==
              std::bit_cast<std::uint64_t>(kFpExpected),
          "fp-result", image.probe->fp_result,
          std::bit_cast<std::uint64_t>(kFpExpected));
  Require(image.probe->variadic_result == kVariadicResult,
          "variadic-upper-bound-result", image.probe->variadic_result,
          kVariadicResult);
  const std::uint64_t sret_address =
      reinterpret_cast<std::uint64_t>(&image.probe->sret);
  Require(image.probe->sret_rax == sret_address &&
              image.probe->sret.first == kSretSeed &&
              image.probe->sret.second == kSretSeed + 1 &&
              image.probe->sret.third == kSretMarker &&
              image.probe->sret.fourth == kSretTail,
          "sret-result", image.probe->sret_rax, sret_address);
  Require(image.probe->before_rsp == image.probe->after_integer_rsp &&
              image.probe->before_rsp == image.probe->after_fp_rsp &&
              image.probe->before_rsp == image.probe->after_variadic_rsp &&
              image.probe->before_rsp == image.probe->after_sret_rsp,
          "guest-rsp-preserved", image.probe->after_sret_rsp,
          image.probe->before_rsp);
  Require(image.probe->integer_continuation_rip ==
                  image.integer_continuation &&
              image.probe->fp_continuation_rip == image.fp_continuation &&
              image.probe->variadic_continuation_rip ==
                  image.variadic_continuation &&
              image.probe->sret_continuation_rip == image.sret_continuation,
          "guest-rip-continuations", image.probe->sret_continuation_rip,
          image.sret_continuation);
  std::fprintf(stderr, "FEX_HLE_GUEST_STUB_ROUNDTRIP STAGE negative-gates\n");
  TestFailClosedMemoryAndVariadicGates(image);
  Require(g_integer_calls.load(std::memory_order_relaxed) == 3 &&
              g_fp_calls.load(std::memory_order_relaxed) == 2 &&
              g_variadic_calls.load(std::memory_order_relaxed) == 2 &&
              g_sret_calls.load(std::memory_order_relaxed) == 3 &&
              g_fatal_calls.load(std::memory_order_relaxed) == 0,
          "production-call-counts");
  Require(api.unbind_host_thread(api.backend_context) == LSX4_FEX_STATUS_OK,
          "unbind-main");

  std::printf(
      "FEX_HLE_GUEST_STUB_ROUNDTRIP PASS guest_asm=5 production_stub=6 "
      "fex_bridge_thunk=14 production_dispatch=14 integer=3 fp=3 sret=3 "
      "variadic_al8_upper_bound=3 variadic_al9_reject=1 "
      "rsp_preserved=12 rip_continuation=12 queued_signal_at_return=1 "
      "pool_wrap_lease=1 "
      "nested_signal_hle_callback=1 nested_signal_outer_drain=1 "
      "stack_path_top32_0=1 stack_path_top32_7=1 pool_path=1 "
      "unmapped_stack_reject=1 unmapped_sret_reject=1 "
      "unregistered_inaccessible_read_reject=1 "
      "unregistered_inaccessible_write_reject=1\n");
  std::fflush(stdout);
  std::_Exit(0);
}
