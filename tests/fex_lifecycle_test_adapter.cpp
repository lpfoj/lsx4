// SPDX-FileCopyrightText: Copyright 2026 LSX4
// SPDX-License-Identifier: GPL-2.0-or-later

#include "executor/fex_backend_abi_v3.h"

#include <cstdlib>
#include <cstring>

namespace {

constexpr std::uint64_t kCapabilities =
    LSX4_FEX_CAP_MAPPED_ENTRY_V5 | LSX4_FEX_CAP_HLE_FRAME_V3 |
    LSX4_FEX_CAP_STRONG_MEMORY | LSX4_FEX_CAP_GUEST_SIGNALS |
    LSX4_FEX_CAP_GUEST_TLS | LSX4_FEX_CAP_GUEST_CALLBACKS |
    LSX4_FEX_CAP_GUEST_FUTEX_SCHEDULING | LSX4_FEX_CAP_SELF_MODIFYING_CODE |
    LSX4_FEX_CAP_EXECUTABLE_RANGE_V5 | LSX4_FEX_CAP_PROCESS_LIFETIME |
    LSX4_FEX_CAP_SINGLE_SIGNAL_OWNER | LSX4_FEX_CAP_EXACT_GUEST_CPU_PROFILE_V6 |
    LSX4_FEX_CAP_MEMORY_MODEL_AB_OVERRIDES;

constexpr std::uint32_t kLifetime =
    LSX4_FEX_LIFETIME_PROCESS_RESIDENT | LSX4_FEX_LIFETIME_ACTIVATE_ONCE |
    LSX4_FEX_LIFETIME_NO_DEACTIVATE | LSX4_FEX_LIFETIME_SINGLE_SIGNAL_OWNER;

std::uint64_t g_activation_epoch{};
std::uint32_t g_configure_flags{};

std::uint64_t EffectiveCapabilities() {
  return g_configure_flags == 0 ? kCapabilities
                                : (kCapabilities & ~LSX4_FEX_CAP_STRONG_MEMORY);
}

int32_t Configure(void *, const Lsx4FexConfigureRequestV3 *request,
                  Lsx4FexConfigureResultV3 *result) {
  if (request == nullptr || result == nullptr) {
    return LSX4_FEX_STATUS_INVALID_ARGUMENT;
  }
  const auto *profile = request->guest_cpu_profile_v6;
  const bool ps5 = request->platform_mask == LSX4_FEX_PLATFORM_PS5;
  const bool profile_identity_matches =
      ps5 ? profile != nullptr &&
                profile->identity_low == UINT64_C(0xca978c3024fac2f4) &&
                profile->identity_high == UINT64_C(0x2e736ab11e7831be)
          : profile != nullptr &&
                profile->identity_low == UINT64_C(0x61e82729ec820a6c) &&
                profile->identity_high == UINT64_C(0xf0e351bb9bbb26d1);
  if (request->struct_size < sizeof(Lsx4FexConfigureRequestV3) ||
      profile == nullptr ||
      profile->struct_size < sizeof(Lsx4FexGuestCpuProfileV6) ||
      profile->abi_version != LSX4_FEX_GUEST_CPU_PROFILE_ABI_V6 ||
      !profile_identity_matches || profile->query_cpuid == nullptr ||
      profile->query_xcr == nullptr ||
      profile->mxcsr_mask != UINT32_C(0x0000ffff) ||
      profile->query_cpuid(profile->profile_context, 0, 0).ebx !=
          UINT32_C(0x68747541) ||
      (ps5 &&
       (profile->query_cpuid(profile->profile_context, UINT32_C(0x80000000), 0)
                .eax != UINT32_C(0x80000008) ||
        (profile->query_cpuid(profile->profile_context, UINT32_C(0x80000008), 0)
             .ecx &
         UINT32_C(0xff)) != UINT32_C(7))) ||
      profile->query_xcr(profile->profile_context, 0) != UINT64_C(7)) {
    result->status = LSX4_FEX_STATUS_ABI_MISMATCH;
    result->lifecycle_state = LSX4_FEX_LIFECYCLE_UNCONFIGURED;
    result->error_message = "exact guest CPU profile contract missing";
    return result->status;
  }
  const char *const mode = std::getenv("LSX4_FEX_LIFECYCLE_TEST_MODE");
  if (mode != nullptr && std::strcmp(mode, "configure-stranded") == 0) {
    result->status = LSX4_FEX_STATUS_OK;
    result->lifecycle_state = LSX4_FEX_LIFECYCLE_ACTIVE;
    result->provided_capabilities = 0;
    result->mapped_entry_abi = LSX4_FEX_MAPPED_ENTRY_ABI_V5;
    result->executable_range_abi = LSX4_FEX_EXECUTABLE_RANGE_ABI_V5;
    result->error_message = "injected invalid active configure result";
    result->request_cookie = request->request_cookie;
    return LSX4_FEX_STATUS_OK;
  }
  g_configure_flags = request->configure_flags;
  result->status = LSX4_FEX_STATUS_OK;
  result->lifecycle_state = LSX4_FEX_LIFECYCLE_CONFIGURED;
  result->provided_capabilities = EffectiveCapabilities();
  result->mapped_entry_abi = LSX4_FEX_MAPPED_ENTRY_ABI_V5;
  result->executable_range_abi = LSX4_FEX_EXECUTABLE_RANGE_ABI_V5;
  result->adapter_name = "lsx4-fex-lifecycle-test";
  result->adapter_version = "1";
  result->strong_memory_model =
      g_configure_flags == 0 ? "test-tso" : "test-relaxed-memory-ab";
  result->error_message = nullptr;
  result->request_cookie = request->request_cookie;
  return LSX4_FEX_STATUS_OK;
}

int32_t Query(void *, const Lsx4FexQueryRequestV3 *request,
              Lsx4FexQueryResultV3 *result) {
  if (request == nullptr || result == nullptr) {
    return LSX4_FEX_STATUS_INVALID_ARGUMENT;
  }
  result->status = LSX4_FEX_STATUS_OK;
  result->lifecycle_state = g_activation_epoch == 0
                                ? LSX4_FEX_LIFECYCLE_CONFIGURED
                                : LSX4_FEX_LIFECYCLE_ACTIVE;
  result->provided_capabilities = EffectiveCapabilities();
  result->activation_epoch = g_activation_epoch;
  result->request_cookie = request->request_cookie;
  return LSX4_FEX_STATUS_OK;
}

int32_t Activate(void *, const Lsx4FexActivateRequestV3 *request,
                 Lsx4FexActivateResultV3 *result) {
  if (request == nullptr || result == nullptr) {
    return LSX4_FEX_STATUS_INVALID_ARGUMENT;
  }
  const char *const mode = std::getenv("LSX4_FEX_LIFECYCLE_TEST_MODE");
  if (mode != nullptr && std::strcmp(mode, "safe-failure") == 0) {
    result->status = LSX4_FEX_STATUS_BACKEND_FAILURE;
    result->lifecycle_state = LSX4_FEX_LIFECYCLE_CONFIGURED;
    result->error_message = "injected safe activation failure";
    result->request_cookie = request->request_cookie;
    return LSX4_FEX_STATUS_BACKEND_FAILURE;
  }

  ++g_activation_epoch;
  result->status = LSX4_FEX_STATUS_OK;
  result->lifecycle_state = LSX4_FEX_LIFECYCLE_ACTIVE;
  result->activation_epoch = g_activation_epoch;
  result->effective_capabilities = EffectiveCapabilities();
  result->error_message = nullptr;
  result->request_cookie = request->request_cookie;
  if (mode != nullptr && std::strcmp(mode, "stranded") == 0) {
    result->effective_capabilities = 0;
    result->error_message = "injected invalid active contract";
  }
  return LSX4_FEX_STATUS_OK;
}

int32_t RunMapped(void *, const Lsx4FexMappedEntryRequestV5 *,
                  Lsx4FexMappedEntryResultV5 *) {
  return LSX4_FEX_STATUS_OK;
}
int32_t RunCallback(void *, const Lsx4FexGuestCallbackRequestV3 *,
                    Lsx4FexGuestCallbackResultV3 *) {
  return LSX4_FEX_STATUS_OK;
}
int32_t RegisterSignal(void *, const Lsx4FexGuestSignalActionV3 *) {
  return LSX4_FEX_STATUS_OK;
}
int32_t QueueSignal(void *, const Lsx4FexGuestSignalDeliveryV3 *) {
  return LSX4_FEX_STATUS_OK;
}
int32_t GetContext(void *, const Lsx4FexThreadIdentityV3 *,
                   Lsx4FexGuestContextV3 *) {
  return LSX4_FEX_STATUS_OK;
}
int32_t SetContext(void *, const Lsx4FexThreadIdentityV3 *,
                   const Lsx4FexGuestContextV3 *) {
  return LSX4_FEX_STATUS_OK;
}
int32_t GetTls(void *, const Lsx4FexThreadIdentityV3 *,
               Lsx4FexGuestTlsStateV3 *) {
  return LSX4_FEX_STATUS_OK;
}
int32_t SetTls(void *, const Lsx4FexGuestTlsStateV3 *) {
  return LSX4_FEX_STATUS_OK;
}
int32_t Bind(void *, const Lsx4FexThreadIdentityV3 *) {
  return LSX4_FEX_STATUS_OK;
}
int32_t Unbind(void *) { return LSX4_FEX_STATUS_OK; }
int32_t ReleaseGuestThread(void *, const Lsx4FexThreadIdentityV3 *) {
  return LSX4_FEX_STATUS_OK;
}
int32_t Snapshot(void *, Lsx4FexExecutableRangeSnapshotV5 *) {
  return LSX4_FEX_STATUS_OK;
}
int32_t BulkRegister(void *, const Lsx4FexExecutableRangeV5 *, std::uint32_t,
                     std::uint64_t generation, std::uint64_t *next) {
  if (next != nullptr)
    *next = generation + 1;
  return LSX4_FEX_STATUS_OK;
}
int32_t Invalidate(void *, const Lsx4FexExecutableRangeV5 *, std::uint32_t,
                   std::uint32_t, std::uint64_t generation,
                   std::uint64_t *next) {
  if (next != nullptr)
    *next = generation + 1;
  return LSX4_FEX_STATUS_OK;
}
int32_t HandleSignal(void *, int32_t, const void *, void *) {
  return LSX4_FEX_HOST_SIGNAL_NOT_HANDLED;
}
int32_t ClassifySignalContextPc(void *, const void *) { return 0; }

} // namespace

extern "C" int32_t lsx4_fex_backend_get_api_v3(const uint32_t requested_abi,
                                               const uint32_t output_size,
                                               Lsx4FexBackendApiV3 *output) {
  if (requested_abi != LSX4_FEX_BACKEND_ABI_V3 || output == nullptr ||
      output_size < sizeof(*output)) {
    return LSX4_FEX_STATUS_ABI_MISMATCH;
  }
  *output = {
      .struct_size = sizeof(*output),
      .abi_version = LSX4_FEX_BACKEND_ABI_V3,
      .provided_capabilities = kCapabilities,
      .lifetime_flags = kLifetime,
      .reserved = 0,
      .backend_context = nullptr,
      .adapter_name = "lsx4-fex-lifecycle-test",
      .adapter_version = "1",
      .configure = Configure,
      .query = Query,
      .activate = Activate,
      .run_mapped_entry_v5 = RunMapped,
      .run_guest_callback = RunCallback,
      .register_guest_signal_action = RegisterSignal,
      .queue_guest_signal = QueueSignal,
      .get_guest_context = GetContext,
      .set_guest_context = SetContext,
      .get_guest_tls = GetTls,
      .set_guest_tls = SetTls,
      .bind_host_thread = Bind,
      .unbind_host_thread = Unbind,
      .snapshot_executable_ranges_v5 = Snapshot,
      .bulk_register_executable_ranges_v5 = BulkRegister,
      .invalidate_executable_ranges_v5 = Invalidate,
      .handle_host_signal = HandleSignal,
      .classify_signal_context_pc = ClassifySignalContextPc,
      .release_guest_thread = ReleaseGuestThread,
  };
  const char *const mode = std::getenv("LSX4_FEX_LIFECYCLE_TEST_MODE");
  if (mode != nullptr && std::strcmp(mode, "preflight-reject") == 0) {
    output->classify_signal_context_pc = nullptr;
  }
  return LSX4_FEX_STATUS_OK;
}
