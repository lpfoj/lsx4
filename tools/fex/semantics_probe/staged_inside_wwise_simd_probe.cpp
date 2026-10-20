// SPDX-License-Identifier: MIT

#include "dynamic_translation/hle_thunk_identity.h"
#include "fex_context_bridge_api.h"
#include "inside_wwise_simd_program.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

#include <asm/hwcap.h>
#include <dlfcn.h>
#include <linux/prctl.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <unistd.h>

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

constexpr std::uint64_t kGuestExitSyscall = UINT64_C(0x5ad00002);
constexpr std::uint64_t kExpectedExit = UINT64_C(0x2608a610);
constexpr std::uint64_t kRequestCookie = UINT64_C(0x575749534553494d);
constexpr std::uint64_t kGuestThread = UINT64_C(0x2608a611);

struct FeatureMode {
  const char *name;
  std::uint64_t feature_mask;
};

// These are the four production-relevant modes.  Every mode deliberately
// leaves SVE256 disabled so YMM operations exercise FEX's split-half lowering.
constexpr std::array<FeatureMode, 4> kFeatureModes{{
    {"baseline", 0},
    {"sve128", LSX4_FEX2608_BRIDGE_SVE128},
    {"afp", LSX4_FEX2608_BRIDGE_AFP},
    {"afp-sve128", LSX4_FEX2608_BRIDGE_AFP | LSX4_FEX2608_BRIDGE_SVE128},
}};

[[noreturn]] void Fail(const char *stage, std::int64_t value = 0) {
  const int saved_errno = errno;
  std::fprintf(stderr,
               "FEX2608_INSIDE_WWISE_SIMD FAIL stage=%s value=%" PRId64
               " errno=%d (%s)\n",
               stage, value, saved_errno, std::strerror(saved_errno));
  std::fflush(stderr);
  std::exit(1);
}

void Require(bool condition, const char *stage, std::int64_t value = 0) {
  if (!condition)
    Fail(stage, value);
}

const FeatureMode &ParseMode(const char *requested) {
  for (const FeatureMode &mode : kFeatureModes) {
    if (std::strcmp(mode.name, requested) == 0)
      return mode;
  }
  std::fprintf(stderr, "usage: lsx4_fex_inside_wwise_simd_probe "
                       "<context-bridge.so> baseline|sve128|afp|afp-sve128\n");
  Fail("feature-mode");
}

void RequirePhysicalFeatures(const FeatureMode &mode) {
  Require((mode.feature_mask & LSX4_FEX2608_BRIDGE_SVE256) == 0,
          "sve256-must-remain-disabled");
  const unsigned long hwcap = getauxval(AT_HWCAP);
  const unsigned long hwcap2 = getauxval(AT_HWCAP2);
  if ((mode.feature_mask & LSX4_FEX2608_BRIDGE_AFP) != 0) {
    Require((hwcap2 & static_cast<unsigned long>(HWCAP2_AFP)) != 0,
            "physical-afp-unavailable", static_cast<std::int64_t>(hwcap2));
  }
  if ((mode.feature_mask & LSX4_FEX2608_BRIDGE_SVE128) != 0) {
    Require((hwcap & static_cast<unsigned long>(HWCAP_SVE)) != 0,
            "physical-sve-unavailable", static_cast<std::int64_t>(hwcap));
    const int sve_vl = prctl(PR_SVE_GET_VL, 0, 0, 0, 0);
    Require(sve_vl >= 0, "physical-sve-vl-query", sve_vl);
    Require((sve_vl & PR_SVE_VL_LEN_MASK) >= 16, "physical-sve128-unavailable",
            sve_vl);
  }
  std::printf("FEX2608_INSIDE_WWISE_SIMD HOST mode=%s requested=0x%016" PRIx64
              " sve256=0 hwcap=0x%016lx hwcap2=0x%016lx\n",
              mode.name, mode.feature_mask, hwcap, hwcap2);
}

std::size_t RoundUp(std::size_t value, std::size_t alignment) {
  return ((value + alignment - 1U) / alignment) * alignment;
}

std::uint32_t CacheLineSize(int name) {
  const long value = sysconf(name);
  return value > 0 && static_cast<unsigned long>(value) <= UINT32_MAX
             ? static_cast<std::uint32_t>(value)
             : UINT32_C(64);
}

std::int32_t RejectUnexpectedHle(void *, const Lsx4FexHleCallFrameV3 *,
                                 Lsx4FexHleResultFrameV3 *) {
  return LSX4_FEX_STATUS_UNSUPPORTED;
}

void ReportFatal(void *, std::int32_t status, const char *message) {
  std::fprintf(stderr,
               "FEX2608_INSIDE_WWISE_SIMD BRIDGE_FATAL status=%d "
               "message=%s\n",
               status, message != nullptr ? message : "-");
}

Lsx4FexCpuidResultV6 QueryPs4Cpuid(const void *, std::uint32_t leaf,
                                   std::uint32_t subleaf) {
  (void)subleaf;
  switch (leaf) {
  case 0:
    return {.eax = UINT32_C(7),
            .ebx = UINT32_C(0x68747541),
            .ecx = UINT32_C(0x444d4163),
            .edx = UINT32_C(0x69746e65)};
  case 1:
    return {.eax = UINT32_C(0x00630f01),
            .ebx = UINT32_C(0x00000800),
            .ecx = UINT32_C(0x3ed80201),
            .edx = UINT32_C(0x07808131)};
  default:
    return {};
  }
}

std::uint64_t QueryPs4Xcr(const void *, std::uint32_t index) {
  return index == 0 ? UINT64_C(7) : 0;
}

void PrintVector(std::size_t index, const Vector &vector) {
  std::printf("FEX2608_INSIDE_WWISE_SIMD_RESULT case=%zu name=%s raw="
              "%08" PRIx32 ",%08" PRIx32 ",%08" PRIx32 ",%08" PRIx32
              ",%08" PRIx32 ",%08" PRIx32 ",%08" PRIx32 ",%08" PRIx32 "\n",
              index, kCaseNames[index], vector[0], vector[1], vector[2],
              vector[3], vector[4], vector[5], vector[6], vector[7]);
}

} // namespace

int main(int argc, char **argv) {
  Require(argc == 3, "usage-context-bridge-and-mode", argc);
  const FeatureMode &mode = ParseMode(argv[2]);
  RequirePhysicalFeatures(mode);

  void *bridge_handle = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
  if (bridge_handle == nullptr) {
    const char *message = dlerror();
    std::fprintf(stderr, "FEX2608_INSIDE_WWISE_SIMD dlerror=%s\n",
                 message != nullptr ? message : "-");
    Fail("dlopen-context-bridge");
  }
  dlerror();
  auto get_api = reinterpret_cast<Lsx4Fex2608GetRuntimeBridgeApiFnV2>(
      dlsym(bridge_handle, LSX4_FEX2608_GET_RUNTIME_BRIDGE_API_V2_SYMBOL));
  const char *symbol_error = dlerror();
  Require(get_api != nullptr && symbol_error == nullptr, "dlsym-runtime-api");

  Lsx4Fex2608RuntimeBridgeApiV2 api{};
  Require(get_api(LSX4_FEX2608_RUNTIME_BRIDGE_ABI_V2, sizeof(api), &api) ==
              LSX4_FEX_STATUS_OK,
          "get-runtime-api");
  Require(
      api.struct_size >= sizeof(api) &&
          api.abi_version == LSX4_FEX2608_RUNTIME_BRIDGE_ABI_V2 &&
          api.create_runtime != nullptr && api.run_mapped_entry != nullptr &&
          api.bind_host_thread != nullptr && api.unbind_host_thread != nullptr,
      "runtime-api-contract");

  const long detected_page_size = sysconf(_SC_PAGESIZE);
  Require(detected_page_size > 0 &&
              static_cast<unsigned long>(detected_page_size) <= UINT32_MAX,
          "page-size", detected_page_size);
  const auto page_size = static_cast<std::size_t>(detected_page_size);
  const std::size_t code_size = RoundUp(kCodeCapacity, page_size);
  const std::size_t stack_size =
      std::max<std::size_t>(UINT64_C(64) * 1024, page_size * 4);
  Require(sizeof(ProbeData) <= page_size, "probe-data-fits-page",
          static_cast<std::int64_t>(sizeof(ProbeData)));

  void *code = mmap(nullptr, code_size, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  Require(code != MAP_FAILED, "code-mmap");
  void *stack = mmap(nullptr, stack_size, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  Require(stack != MAP_FAILED, "stack-mmap");
  void *data_mapping = mmap(nullptr, page_size, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  Require(data_mapping != MAP_FAILED, "data-mmap");
  auto *data = ::new (data_mapping) ProbeData{};
  Require(EmitProgram(code, code_size, data, false, kGuestExitSyscall,
                      static_cast<std::uint32_t>(kExpectedExit)),
          "guest-code-overflow");
  Require(mprotect(code, code_size, PROT_READ | PROT_EXEC) == 0,
          "code-mprotect-rx");

  Lsx4FexExecutableRangeV5 executable_range{};
  executable_range.address = reinterpret_cast<std::uint64_t>(code);
  executable_range.size = code_size;
  executable_range.range_cookie = UINT64_C(0x5757495345434f44);
  executable_range.protection = PROT_READ | PROT_EXEC;
  executable_range.range_flags = LSX4_FEX_RANGE_EXECUTABLE;

  Lsx4FexExecutableRangeSnapshotV5 range_snapshot{};
  range_snapshot.struct_size = sizeof(range_snapshot);
  range_snapshot.abi_version = LSX4_FEX_EXECUTABLE_RANGE_ABI_V5;
  range_snapshot.generation = 1;
  range_snapshot.ranges = &executable_range;
  range_snapshot.capacity = 1;
  range_snapshot.range_count = 1;

  Lsx4FexHostCallbacksV3 callbacks{};
  callbacks.struct_size = sizeof(callbacks);
  callbacks.abi_version = LSX4_FEX_BACKEND_ABI_V3;
  callbacks.dispatch_hle = RejectUnexpectedHle;
  callbacks.fatal_error = ReportFatal;

  const Lsx4FexGuestCpuProfileV6 guest_cpu_profile{
      .struct_size = sizeof(Lsx4FexGuestCpuProfileV6),
      .abi_version = LSX4_FEX_GUEST_CPU_PROFILE_ABI_V6,
      .identity_low = UINT64_C(0x805bd89575c14c34),
      .identity_high = UINT64_C(0x923fa04d7677e310),
      .profile_id = "lsx4-inside-wwise-simd-probe-v1",
      .profile_context = nullptr,
      .query_cpuid = QueryPs4Cpuid,
      .query_xcr = QueryPs4Xcr,
      .xcr0_writable_mask = 0,
      .mxcsr_reset_value = UINT32_C(0x00001f80),
      .mxcsr_mask = UINT32_C(0x0000ffff),
      .legacy_save_area_bytes = 512,
      .profile_flags = 0,
  };

  Lsx4Fex2608RuntimeCreateRequestV2 create_request{};
  create_request.struct_size = sizeof(create_request);
  create_request.abi_version = LSX4_FEX2608_RUNTIME_BRIDGE_ABI_V2;
  create_request.host_features.struct_size =
      sizeof(create_request.host_features);
  create_request.host_features.abi_version = LSX4_FEX2608_CONTEXT_BRIDGE_ABI;
  create_request.host_features.feature_mask = mode.feature_mask;
  create_request.host_features.dcache_line_size =
      CacheLineSize(_SC_LEVEL1_DCACHE_LINESIZE);
  create_request.host_features.icache_line_size =
      CacheLineSize(_SC_LEVEL1_ICACHE_LINESIZE);
  create_request.host_features.sve_vector_bytes =
      (mode.feature_mask & LSX4_FEX2608_BRIDGE_SVE128) != 0 ? 16U : 0U;
  create_request.host_features.page_size =
      static_cast<std::uint32_t>(page_size);
  create_request.strong_memory_mode = LSX4_FEX2608_STRONG_MEMORY_SOFTWARE_TSO;
  create_request.create_flags = LSX4_FEX2608_CREATE_STAGED_PRIVATE_HLT_EXIT;
  create_request.host_callbacks = &callbacks;
  create_request.hle_thunk_identity =
      Lsx4::Translation::FexThunkIdentity.data();
  create_request.hle_thunk_identity_size =
      static_cast<std::uint32_t>(Lsx4::Translation::FexThunkIdentity.size());
  create_request.initial_executable_ranges = &range_snapshot;
  create_request.request_cookie = kRequestCookie;
  create_request.guest_cpu_profile_v6 = &guest_cpu_profile;

  Lsx4Fex2608RuntimeCreateResultV2 create_result{};
  create_result.struct_size = sizeof(create_result);
  create_result.abi_version = LSX4_FEX2608_RUNTIME_BRIDGE_ABI_V2;
  const std::int32_t create_status =
      api.create_runtime(&create_request, &create_result);
  if (create_status != LSX4_FEX_STATUS_OK ||
      create_result.status != LSX4_FEX_STATUS_OK ||
      create_result.runtime == nullptr ||
      create_result.request_cookie != kRequestCookie) {
    std::fprintf(
        stderr,
        "FEX2608_INSIDE_WWISE_SIMD create status=%d result=%d "
        "features=0x%" PRIx64 " diagnostics=0x%" PRIx64 " message=%s\n",
        create_status, create_result.status, create_result.runtime_features,
        create_result.diagnostics,
        create_result.error_message != nullptr ? create_result.error_message
                                               : "-");
    Fail("create-staged-runtime", create_status);
  }

  Lsx4FexThreadIdentityV3 identity{};
  identity.struct_size = sizeof(identity);
  identity.abi_version = LSX4_FEX_BACKEND_ABI_V3;
  identity.guest_thread = kGuestThread;
  Require(api.bind_host_thread(create_result.runtime, &identity) ==
              LSX4_FEX_STATUS_OK,
          "bind-host-thread");

  Lsx4FexMappedSegmentV5 segments[3]{};
  segments[0].address = reinterpret_cast<std::uint64_t>(code);
  segments[0].size = code_size;
  segments[0].range_cookie = executable_range.range_cookie;
  segments[0].protection = PROT_READ | PROT_EXEC;
  segments[0].range_flags = LSX4_FEX_RANGE_EXECUTABLE;
  segments[1].address = reinterpret_cast<std::uint64_t>(stack);
  segments[1].size = stack_size;
  segments[1].range_cookie = UINT64_C(0x575749534553544b);
  segments[1].protection = PROT_READ | PROT_WRITE;
  segments[2].address = reinterpret_cast<std::uint64_t>(data_mapping);
  segments[2].size = page_size;
  segments[2].range_cookie = UINT64_C(0x5757495345444154);
  segments[2].protection = PROT_READ | PROT_WRITE;

  Lsx4FexMappedEntryRequestV5 mapped_entry{};
  mapped_entry.struct_size = sizeof(mapped_entry);
  mapped_entry.abi_version = LSX4_FEX_MAPPED_ENTRY_ABI_V5;
  mapped_entry.entry = reinterpret_cast<std::uint64_t>(code);
  mapped_entry.stack_base = reinterpret_cast<std::uint64_t>(stack);
  mapped_entry.stack_size = stack_size;
  mapped_entry.tls_module_id = 1;
  mapped_entry.tcb_base = reinterpret_cast<std::uint64_t>(stack) + 0x100;
  mapped_entry.segments = segments;
  mapped_entry.segment_count = 3;
  mapped_entry.executable_ranges = &range_snapshot;
  mapped_entry.request_cookie = kRequestCookie;

  Lsx4Fex2608RunMappedEntryRequestV2 run_request{};
  run_request.struct_size = sizeof(run_request);
  run_request.abi_version = LSX4_FEX2608_RUNTIME_BRIDGE_ABI_V2;
  run_request.mapped_entry = &mapped_entry;

  Lsx4Fex2608RunMappedEntryResultV2 run_result{};
  run_result.struct_size = sizeof(run_result);
  run_result.abi_version = LSX4_FEX2608_RUNTIME_BRIDGE_ABI_V2;
  const std::int32_t run_status =
      api.run_mapped_entry(create_result.runtime, &run_request, &run_result);
  const std::int32_t unbind_status =
      api.unbind_host_thread(create_result.runtime);
  if (run_status != LSX4_FEX_STATUS_OK ||
      run_result.mapped_entry.status != LSX4_FEX_STATUS_OK ||
      run_result.mapped_entry.request_cookie != kRequestCookie ||
      run_result.mapped_entry.guest_exit_code != kExpectedExit ||
      run_result.final_context.guest_thread != kGuestThread) {
    std::fprintf(stderr,
                 "FEX2608_INSIDE_WWISE_SIMD run status=%d result=%d "
                 "exit=0x%" PRIx64 " expected=0x%" PRIx64 " rip=0x%" PRIx64
                 " thread=0x%" PRIx64 "\n",
                 run_status, run_result.mapped_entry.status,
                 run_result.mapped_entry.guest_exit_code, kExpectedExit,
                 run_result.mapped_entry.final_rip,
                 run_result.final_context.guest_thread);
    Fail("run-mapped-entry", run_status);
  }
  Require(unbind_status == LSX4_FEX_STATUS_OK, "unbind-host-thread",
          unbind_status);

  for (std::size_t index = 0; index < kCaseCount; ++index) {
    PrintVector(index, data->results[index]);
    Require(data->results[index] == kExpectedResults[index], kCaseNames[index],
            static_cast<std::int64_t>(index));
  }
  const std::uint64_t digest = Digest(*data);
  Require(digest == kExpectedDigest, "x86-oracle-digest",
          static_cast<std::int64_t>(digest));

  data->~ProbeData();
  Require(munmap(data_mapping, page_size) == 0, "data-munmap");
  Require(munmap(stack, stack_size) == 0, "stack-munmap");
  Require(munmap(code, code_size) == 0, "code-munmap");
  std::printf("FEX2608_INSIDE_WWISE_SIMD PASS mode=%s featureMask=0x%016" PRIx64
              " sve256=0 cases=%zu digest=%016" PRIx64 "\n",
              mode.name, mode.feature_mask, kCaseCount, digest);
  std::fflush(stdout);
  return 0;
}
