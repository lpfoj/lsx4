// SPDX-License-Identifier: MIT

#include "dynamic_translation/hle_thunk_identity.h"
#include "fex_context_bridge_api.h"
#include "memory_tso_program.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <thread>

#include <dlfcn.h>
#include <sys/mman.h>
#include <unistd.h>

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

constexpr std::uint64_t kGuestExitSyscall = UINT64_C(0x5ad00002);
constexpr std::uint32_t kDeterministicExit = UINT32_C(0x2608b500);
constexpr std::uint32_t kPublicationExitBase = UINT32_C(0x2608b510);
constexpr std::uint64_t kCreateCookie = UINT64_C(0x4d454d54534f4352);
constexpr std::uint64_t kCodeCookie = UINT64_C(0x4d454d54534f4344);
constexpr std::uint64_t kDataCookie = UINT64_C(0x4d454d54534f4441);
constexpr std::size_t kStackCount = 3;

struct MemoryMode {
  const char *name;
  std::uint32_t create_flag;
  const char *memory_model;
  bool relaxed;
};

constexpr std::array<MemoryMode, 3> kModes{{
    {"baseline", 0,
     "FEX software TSO (hardware TSO when enabled by the host OS)", false},
    {"memcpy-relaxed", LSX4_FEX2608_CREATE_RELAX_MEMCPY_SET_TSO_AB,
     "FEX software TSO (MemcpySetTSO relaxed A/B)", true},
    {"vector-relaxed", LSX4_FEX2608_CREATE_RELAX_VECTOR_TSO_AB,
     "FEX software TSO (VectorTSO relaxed A/B)", true},
}};

[[noreturn]] void Fail(const char *stage, std::int64_t value = 0) {
  const int saved_errno = errno;
  std::fprintf(stderr,
               "FEX2608_MEMORY_TSO FAIL stage=%s value=%" PRId64
               " errno=%d (%s)\n",
               stage, value, saved_errno, std::strerror(saved_errno));
  std::fflush(stderr);
  std::exit(1);
}

void Require(bool condition, const char *stage, std::int64_t value = 0) {
  if (!condition)
    Fail(stage, value);
}

const MemoryMode &ParseMode(const char *requested) {
  for (const MemoryMode &mode : kModes) {
    if (std::strcmp(requested, mode.name) == 0)
      return mode;
  }
  std::fprintf(stderr, "usage: lsx4_fex_memory_tso_probe <context-bridge.so> "
                       "baseline|memcpy-relaxed|vector-relaxed\n");
  Fail("memory-mode");
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
  std::fprintf(stderr, "FEX2608_MEMORY_TSO BRIDGE_FATAL status=%d message=%s\n",
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

struct Harness {
  const Lsx4Fex2608RuntimeBridgeApiV2 *api{};
  void *runtime{};
  std::uint8_t *code{};
  std::size_t code_size{};
  std::uint8_t *stacks{};
  std::size_t stack_size{};
  ProbeData *data{};
  std::size_t data_size{};
  const Lsx4FexExecutableRangeSnapshotV5 *range_snapshot{};
};

struct RunSpec {
  std::size_t entry_offset{};
  std::size_t stack_index{};
  std::uint64_t guest_thread{};
  std::uint64_t cookie{};
  std::uint32_t expected_exit{};
};

struct StartGate {
  std::atomic<std::uint32_t> ready{};
  std::atomic<bool> go{};
};

std::int32_t RunGuest(const Harness &harness, const RunSpec &spec,
                      StartGate *gate = nullptr) {
  Lsx4FexThreadIdentityV3 identity{};
  identity.struct_size = sizeof(identity);
  identity.abi_version = LSX4_FEX_BACKEND_ABI_V3;
  identity.guest_thread = spec.guest_thread;
  std::int32_t status =
      harness.api->bind_host_thread(harness.runtime, &identity);
  const bool bound = status == LSX4_FEX_STATUS_OK;

  if (gate != nullptr) {
    gate->ready.fetch_add(1, std::memory_order_release);
    while (!gate->go.load(std::memory_order_acquire))
      std::this_thread::yield();
  }

  Lsx4FexMappedEntryResultV5 mapped_result{};
  if (bound) {
    auto *stack = harness.stacks + spec.stack_index * harness.stack_size;
    Lsx4FexMappedSegmentV5 segments[3]{};
    segments[0].address = reinterpret_cast<std::uint64_t>(harness.code);
    segments[0].size = harness.code_size;
    segments[0].range_cookie = kCodeCookie;
    segments[0].protection = PROT_READ | PROT_EXEC;
    segments[0].range_flags = LSX4_FEX_RANGE_EXECUTABLE;
    segments[1].address = reinterpret_cast<std::uint64_t>(stack);
    segments[1].size = harness.stack_size;
    segments[1].range_cookie = spec.cookie ^ UINT64_C(0x535441434b);
    segments[1].protection = PROT_READ | PROT_WRITE;
    segments[2].address = reinterpret_cast<std::uint64_t>(harness.data);
    segments[2].size = harness.data_size;
    segments[2].range_cookie = kDataCookie;
    segments[2].protection = PROT_READ | PROT_WRITE;

    Lsx4FexMappedEntryRequestV5 mapped_entry{};
    mapped_entry.struct_size = sizeof(mapped_entry);
    mapped_entry.abi_version = LSX4_FEX_MAPPED_ENTRY_ABI_V5;
    mapped_entry.timeout_ms = 30'000;
    mapped_entry.entry =
        reinterpret_cast<std::uint64_t>(harness.code) + spec.entry_offset;
    mapped_entry.stack_base = reinterpret_cast<std::uint64_t>(stack);
    mapped_entry.stack_size = harness.stack_size;
    mapped_entry.tls_module_id = spec.guest_thread;
    mapped_entry.tcb_base = reinterpret_cast<std::uint64_t>(stack) + 0x100;
    mapped_entry.segments = segments;
    mapped_entry.segment_count = 3;
    mapped_entry.executable_ranges = harness.range_snapshot;
    mapped_entry.request_cookie = spec.cookie;

    Lsx4Fex2608RunMappedEntryRequestV2 run_request{};
    run_request.struct_size = sizeof(run_request);
    run_request.abi_version = LSX4_FEX2608_RUNTIME_BRIDGE_ABI_V2;
    run_request.mapped_entry = &mapped_entry;

    Lsx4Fex2608RunMappedEntryResultV2 run_result{};
    run_result.struct_size = sizeof(run_result);
    run_result.abi_version = LSX4_FEX2608_RUNTIME_BRIDGE_ABI_V2;
    status = harness.api->run_mapped_entry(harness.runtime, &run_request,
                                           &run_result);
    mapped_result = run_result.mapped_entry;
    if (status == LSX4_FEX_STATUS_OK &&
        (mapped_result.status != LSX4_FEX_STATUS_OK ||
         mapped_result.request_cookie != spec.cookie ||
         mapped_result.guest_exit_code != spec.expected_exit ||
         run_result.final_context.guest_thread != spec.guest_thread)) {
      status = mapped_result.status != LSX4_FEX_STATUS_OK
                   ? mapped_result.status
                   : LSX4_FEX_STATUS_BACKEND_FAILURE;
    }
  }

  if (bound) {
    const std::int32_t unbind =
        harness.api->unbind_host_thread(harness.runtime);
    if (status == LSX4_FEX_STATUS_OK && unbind != LSX4_FEX_STATUS_OK)
      status = unbind;
    if (harness.api->release_guest_thread != nullptr) {
      const std::int32_t release =
          harness.api->release_guest_thread(harness.runtime, &identity);
      if (status == LSX4_FEX_STATUS_OK && release != LSX4_FEX_STATUS_OK)
        status = release;
    }
  }

  if (status != LSX4_FEX_STATUS_OK) {
    std::fprintf(stderr,
                 "FEX2608_MEMORY_TSO run status=%d guest=%" PRIu64
                 " entry=%zu result=%d exit=0x%" PRIx64 " expected=0x%08" PRIx32
                 " rip=0x%" PRIx64 "\n",
                 status, spec.guest_thread, spec.entry_offset,
                 mapped_result.status, mapped_result.guest_exit_code,
                 spec.expected_exit, mapped_result.final_rip);
  }
  return status;
}

void WaitForReady(StartGate &gate) {
  for (std::uint32_t elapsed = 0; elapsed < 10'000; ++elapsed) {
    if (gate.ready.load(std::memory_order_acquire) == 2)
      return;
    usleep(1000);
  }
  Fail("publication-host-ready-timeout");
}

void RunPublication(const Harness &harness, PublicationKind kind,
                    std::uint64_t guest_thread_base) {
  ResetPublicationData(harness.data->publication);
  const std::uint32_t value = static_cast<std::uint32_t>(kind);
  const RunSpec producer{
      .entry_offset = Lsx4::FexMemoryTsoProbe::ProducerEntryOffset(kind),
      .stack_index = 1,
      .guest_thread = guest_thread_base,
      .cookie = UINT64_C(0x50524f4400000000) | value,
      .expected_exit = kPublicationExitBase + value * 2U,
  };
  const RunSpec consumer{
      .entry_offset = Lsx4::FexMemoryTsoProbe::ConsumerEntryOffset(kind),
      .stack_index = 2,
      .guest_thread = guest_thread_base + 1U,
      .cookie = UINT64_C(0x434f4e5300000000) | value,
      .expected_exit = kPublicationExitBase + value * 2U + 1U,
  };
  StartGate gate{};
  std::atomic<std::int32_t> producer_status{LSX4_FEX_STATUS_INVALID_STATE};
  std::atomic<std::int32_t> consumer_status{LSX4_FEX_STATUS_INVALID_STATE};
  std::thread producer_thread{[&] {
    producer_status.store(RunGuest(harness, producer, &gate),
                          std::memory_order_release);
  }};
  std::thread consumer_thread{[&] {
    consumer_status.store(RunGuest(harness, consumer, &gate),
                          std::memory_order_release);
  }};
  WaitForReady(gate);
  gate.go.store(true, std::memory_order_release);
  producer_thread.join();
  consumer_thread.join();
  Require(producer_status.load(std::memory_order_acquire) == LSX4_FEX_STATUS_OK,
          "publication-producer-run", producer_status.load());
  Require(consumer_status.load(std::memory_order_acquire) == LSX4_FEX_STATUS_OK,
          "publication-consumer-run", consumer_status.load());
  if (!ValidatePublicationData(harness.data->publication, kind)) {
    std::fprintf(
        stderr,
        "FEX2608_MEMORY_TSO publication mismatch kind=%s published=%" PRIu64
        " ack=%" PRIu64 " failures=%" PRIu64 " producerTimeout=%" PRIu64
        " consumerTimeout=%" PRIu64 "\n",
        PublicationKindName(kind), harness.data->publication.published,
        harness.data->publication.acknowledged,
        harness.data->publication.failures,
        harness.data->publication.producer_timeout,
        harness.data->publication.consumer_timeout);
    Fail(PublicationKindName(kind));
  }
  std::printf("FEX2608_MEMORY_TSO kind=%s iterations=%u PASS\n",
              PublicationKindName(kind),
              Lsx4::FexMemoryTsoProbe::kPublicationIterations);
}

} // namespace

int main(int argc, char **argv) {
  Require(argc == 3, "usage-context-bridge-and-mode", argc);
  const MemoryMode &mode = ParseMode(argv[2]);

  void *bridge_handle = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
  if (bridge_handle == nullptr) {
    const char *message = dlerror();
    std::fprintf(stderr, "FEX2608_MEMORY_TSO dlerror=%s\n",
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
  Require(api.struct_size >= sizeof(api) &&
              api.abi_version == LSX4_FEX2608_RUNTIME_BRIDGE_ABI_V2 &&
              api.create_runtime != nullptr &&
              api.run_mapped_entry != nullptr &&
              api.bind_host_thread != nullptr &&
              api.unbind_host_thread != nullptr && api.query_runtime != nullptr,
          "runtime-api-contract");

  const long detected_page_size = sysconf(_SC_PAGESIZE);
  Require(detected_page_size > 0 &&
              static_cast<unsigned long>(detected_page_size) <= UINT32_MAX,
          "page-size", detected_page_size);
  const auto page_size = static_cast<std::size_t>(detected_page_size);
  const std::size_t code_size = RoundUp(kCodeCapacity, page_size);
  const std::size_t data_size = RoundUp(sizeof(ProbeData), page_size);
  const std::size_t stack_size =
      std::max<std::size_t>(UINT64_C(64) * 1024, page_size * 4);

  void *code_mapping = mmap(nullptr, code_size, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  Require(code_mapping != MAP_FAILED, "code-mmap");
  void *data_mapping = mmap(nullptr, data_size, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  Require(data_mapping != MAP_FAILED, "data-mmap");
  void *stack_mapping =
      mmap(nullptr, stack_size * kStackCount, PROT_READ | PROT_WRITE,
           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  Require(stack_mapping != MAP_FAILED, "stack-mmap");

  auto *data = ::new (data_mapping) ProbeData{};
  InitializeDeterministicData(data->deterministic);
  Require(EmitAllPrograms(code_mapping, code_size, data, false,
                          kGuestExitSyscall, kDeterministicExit,
                          kPublicationExitBase),
          "guest-code-overflow");
  Require(mprotect(code_mapping, code_size, PROT_READ | PROT_EXEC) == 0,
          "code-mprotect-rx");

  Lsx4FexExecutableRangeV5 executable_range{};
  executable_range.address = reinterpret_cast<std::uint64_t>(code_mapping);
  executable_range.size = code_size;
  executable_range.range_cookie = kCodeCookie;
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
      .identity_low = UINT64_C(0x2e74124cb693a51d),
      .identity_high = UINT64_C(0xb4055e71620ca893),
      .profile_id = "lsx4-fex-memory-tso-probe-v1",
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
  // Deliberately expose the architectural baseline. This exercises FEX's
  // software-TSO lowering without relying on device-specific LRCPC/LSE paths.
  create_request.host_features.feature_mask = 0;
  create_request.host_features.dcache_line_size =
      CacheLineSize(_SC_LEVEL1_DCACHE_LINESIZE);
  create_request.host_features.icache_line_size =
      CacheLineSize(_SC_LEVEL1_ICACHE_LINESIZE);
  create_request.host_features.page_size =
      static_cast<std::uint32_t>(page_size);
  create_request.strong_memory_mode = LSX4_FEX2608_STRONG_MEMORY_SOFTWARE_TSO;
  create_request.create_flags =
      LSX4_FEX2608_CREATE_STAGED_PRIVATE_HLT_EXIT | mode.create_flag;
  create_request.host_callbacks = &callbacks;
  create_request.hle_thunk_identity =
      Lsx4::Translation::FexThunkIdentity.data();
  create_request.hle_thunk_identity_size =
      static_cast<std::uint32_t>(Lsx4::Translation::FexThunkIdentity.size());
  create_request.initial_executable_ranges = &range_snapshot;
  create_request.request_cookie = kCreateCookie;
  create_request.guest_cpu_profile_v6 = &guest_cpu_profile;

  Lsx4Fex2608RuntimeCreateResultV2 create_result{};
  create_result.struct_size = sizeof(create_result);
  create_result.abi_version = LSX4_FEX2608_RUNTIME_BRIDGE_ABI_V2;
  const std::int32_t create_status =
      api.create_runtime(&create_request, &create_result);
  if (create_status != LSX4_FEX_STATUS_OK ||
      create_result.status != LSX4_FEX_STATUS_OK ||
      create_result.runtime == nullptr ||
      create_result.request_cookie != kCreateCookie) {
    std::fprintf(
        stderr,
        "FEX2608_MEMORY_TSO create status=%d result=%d "
        "features=0x%" PRIx64 " diagnostics=0x%" PRIx64 " message=%s\n",
        create_status, create_result.status, create_result.runtime_features,
        create_result.diagnostics,
        create_result.error_message != nullptr ? create_result.error_message
                                               : "-");
    Fail("create-staged-runtime", create_status);
  }

  Lsx4Fex2608RuntimeInfoV2 runtime_info{};
  runtime_info.struct_size = sizeof(runtime_info);
  runtime_info.abi_version = LSX4_FEX2608_RUNTIME_BRIDGE_ABI_V2;
  Require(api.query_runtime(create_result.runtime, &runtime_info) ==
                  LSX4_FEX_STATUS_OK &&
              runtime_info.status == LSX4_FEX_STATUS_OK &&
              runtime_info.strong_memory_name != nullptr &&
              std::strcmp(runtime_info.strong_memory_name, mode.memory_model) ==
                  0,
          "runtime-memory-model-provenance");
  const bool has_strong =
      (runtime_info.runtime_features & LSX4_FEX2608_RUNTIME_STRONG_MEMORY) != 0;
  const bool has_relaxed = (runtime_info.runtime_features &
                            LSX4_FEX2608_RUNTIME_RELAXED_MEMORY_MODEL_AB) != 0;
  Require(mode.relaxed ? (!has_strong && has_relaxed)
                       : (has_strong && !has_relaxed),
          "runtime-memory-feature-provenance",
          static_cast<std::int64_t>(runtime_info.runtime_features));
  std::printf("FEX2608_MEMORY_TSO runtime mode=%s createFlag=0x%08" PRIx32
              " model=%s features=0x%016" PRIx64 "\n",
              mode.name, mode.create_flag, runtime_info.strong_memory_name,
              runtime_info.runtime_features);

  const Harness harness{
      .api = &api,
      .runtime = create_result.runtime,
      .code = static_cast<std::uint8_t *>(code_mapping),
      .code_size = code_size,
      .stacks = static_cast<std::uint8_t *>(stack_mapping),
      .stack_size = stack_size,
      .data = data,
      .data_size = data_size,
      .range_snapshot = &range_snapshot,
  };
  const RunSpec deterministic{
      .entry_offset = Lsx4::FexMemoryTsoProbe::DeterministicEntryOffset(),
      .stack_index = 0,
      .guest_thread = UINT64_C(0x2608b501),
      .cookie = UINT64_C(0x44455445524d494e),
      .expected_exit = kDeterministicExit,
  };
  Require(RunGuest(harness, deterministic) == LSX4_FEX_STATUS_OK,
          "deterministic-run");
  Require(ValidateDeterministicData(data->deterministic),
          "deterministic-rep-semantics");
  const std::uint64_t digest = DigestDeterministicData(data->deterministic);
  Require(digest == Lsx4::FexMemoryTsoProbe::kExpectedDeterministicDigest,
          "deterministic-digest", static_cast<std::int64_t>(digest));
  std::printf("FEX2608_MEMORY_TSO deterministic=PASS digest=%016" PRIx64 "\n",
              digest);

  RunPublication(harness, PublicationKind::RepMovs, UINT64_C(0x2608b520));
  RunPublication(harness, PublicationKind::RepStos, UINT64_C(0x2608b530));
  RunPublication(harness, PublicationKind::VectorStore, UINT64_C(0x2608b540));

  data->~ProbeData();
  Require(munmap(stack_mapping, stack_size * kStackCount) == 0, "stack-munmap");
  Require(munmap(data_mapping, data_size) == 0, "data-munmap");
  Require(munmap(code_mapping, code_size) == 0, "code-munmap");
  std::printf(
      "FEX2608_MEMORY_TSO PASS mode=%s deterministic=1 "
      "publicationKinds=3 iterationsPerKind=%u hostFeatureProfile=baseline\n",
      mode.name, Lsx4::FexMemoryTsoProbe::kPublicationIterations);
  std::fflush(stdout);
  return 0;
}
