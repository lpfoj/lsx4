// SPDX-FileCopyrightText: Copyright 2026 LSX4
// SPDX-License-Identifier: GPL-2.0-or-later

#include "executor/fex_backend_loader.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

[[noreturn]] void Fail(const char *message) {
  std::fprintf(stderr, "fex_backend_loader_lifecycle_smoke: %s\n", message);
  std::exit(1);
}

void Require(const bool condition, const char *message) {
  if (!condition)
    Fail(message);
}

int32_t Dispatch(void *, const Lsx4FexHleCallFrameV3 *,
                 Lsx4FexHleResultFrameV3 *) {
  return LSX4_FEX_STATUS_OK;
}

void Fatal(void *, int32_t, const char *) {}

} // namespace

int main(const int argc, char **argv) {
  if (argc != 3) {
    Fail("usage: <adapter.so> "
         "<normal|safe-failure|stranded|configure-stranded|commit-before-stage|"
         "preflight-reject|"
         "profile-bypass|preflight-mismatch|relaxed-vector|relaxed-memcpy|"
         "relaxed-combined|"
         "relaxed-explicit-strong|ps5-profile>");
  }
  const char *const scenario = argv[2];
  setenv("LSX4_FEX_LIFECYCLE_TEST_MODE", scenario, 1);

  const auto ps4_namespace = Executor::Fex::BuildPlatformCacheNamespace(
      "fex-2608", LSX4_FEX_PLATFORM_PS4,
      Executor::Fex::GuestCpuProfileKind::Ps4Jaguar, 3, UINT64_C(0x1234), 16,
      "-test");
  const auto ps5_namespace = Executor::Fex::BuildPlatformCacheNamespace(
      "fex-2608", LSX4_FEX_PLATFORM_PS5,
      Executor::Fex::GuestCpuProfileKind::Ps5Zen2, 1, UINT64_C(0x1234), 16,
      "-test");
  Require(ps4_namespace == "fex-2608-ps4-v3-cpu-61e82729ec820a6c-"
                           "f0e351bb9bbb26d1-isa0000000000001234-vl16-test",
          "PS4 cache namespace lost its profile identity");
  Require(ps5_namespace == "fex-2608-ps5-v1-cpu-ca978c3024fac2f4-"
                           "2e736ab11e7831be-isa0000000000001234-vl16-test",
          "PS5 cache namespace lost its profile identity");
  Require(ps4_namespace != ps5_namespace,
          "PS4 and PS5 cache namespaces collided");
  Require(Executor::Fex::BuildPlatformCacheNamespace(
              "fex-2608", LSX4_FEX_PLATFORM_PS5,
              Executor::Fex::GuestCpuProfileKind::Ps4Jaguar, 1, 0, 0)
              .empty(),
          "mismatched platform/profile built a cache namespace");

  Executor::Fex::PreflightRequest preflight{};
  preflight.adapter_path = argv[1];
  preflight.runtime_root = "/data/local/tmp";
  preflight.cache_namespace = "fex-lifecycle-smoke";
  preflight.host_build_id = "fex-lifecycle-smoke";
  if (std::strcmp(scenario, "ps5-profile") == 0) {
    preflight.platform_mask = LSX4_FEX_PLATFORM_PS5;
    preflight.guest_cpu_profile = Executor::Fex::GuestCpuProfileKind::Ps5Zen2;
  } else if (std::strcmp(scenario, "relaxed-vector") == 0) {
    preflight.configure_flags = LSX4_FEX_CONFIG_RELAX_VECTOR_TSO_AB;
  } else if (std::strcmp(scenario, "relaxed-memcpy") == 0) {
    preflight.configure_flags = LSX4_FEX_CONFIG_RELAX_MEMCPY_SET_TSO_AB;
  } else if (std::strcmp(scenario, "relaxed-combined") == 0) {
    preflight.configure_flags = LSX4_FEX_CONFIG_RELAX_VECTOR_TSO_AB |
                                LSX4_FEX_CONFIG_RELAX_MEMCPY_SET_TSO_AB;
  } else if (std::strcmp(scenario, "relaxed-explicit-strong") == 0) {
    preflight.configure_flags = LSX4_FEX_CONFIG_RELAX_VECTOR_TSO_AB;
    preflight.required_capabilities =
        LSX4_FEX_CAP_MAPPED_ENTRY_V5 | LSX4_FEX_CAP_HLE_FRAME_V3 |
        LSX4_FEX_CAP_STRONG_MEMORY | LSX4_FEX_CAP_EXACT_GUEST_CPU_PROFILE_V6;
    Require(!Executor::Fex::Preflight(preflight),
            "relaxed override accepted an explicit strong-memory request");
    Require(Executor::Fex::Status().error ==
                "relaxed-memory-ab-conflicts-with-explicit-strong-memory",
            "explicit strong-memory conflict reported the wrong reason");
    return 0;
  }
  if (std::strcmp(scenario, "profile-bypass") == 0) {
    preflight.required_capabilities = LSX4_FEX_CAP_MAPPED_ENTRY_V5;
    Require(Executor::Fex::Preflight(preflight),
            "additive PS4 preflight failed");
    const auto status = Executor::Fex::Status();
    Require((status.required_capabilities &
             LSX4_FEX_CAP_EXACT_GUEST_CPU_PROFILE_V6) != 0,
            "partial caller mask weakened the PS4 CPU-profile contract");
    Require((status.required_capabilities & LSX4_FEX_CAP_SINGLE_SIGNAL_OWNER) !=
                0,
            "partial caller mask weakened the PS4 signal-owner contract");
    std::printf("FEX_LOADER_LIFECYCLE PASS scenario=profile-bypass code=%d "
                "reason=%s\n",
                status.code, status.error.c_str());
    return 0;
  }
  const Lsx4FexHostCallbacksV3 callbacks{
      .struct_size = sizeof(Lsx4FexHostCallbacksV3),
      .abi_version = LSX4_FEX_BACKEND_ABI_V3,
      .callback_context = nullptr,
      .log = nullptr,
      .dispatch_hle = Dispatch,
      .thread_event = nullptr,
      .fatal_error = Fatal,
  };
  const bool preflight_ok = Executor::Fex::Preflight(preflight);
  if (std::strcmp(scenario, "configure-stranded") == 0) {
    Require(!preflight_ok,
            "invalid ACTIVE configure result unexpectedly passed preflight");
    Require(Executor::Fex::Status().state ==
                Executor::Fex::LoaderState::ActivationStranded,
            "ACTIVE configure result was not quarantined");
    Require(Executor::Fex::ActiveApi() == nullptr,
            "normal API leaked from configure quarantine");
    Require(Executor::Fex::ActiveApiForSignal() == nullptr,
            "signal API leaked from configure quarantine");
    Require(!Executor::Fex::StageActivation(callbacks, nullptr),
            "stage escaped configure quarantine");
    Require(!Executor::Fex::CommitActivation(),
            "commit escaped configure quarantine");
    Require(Executor::Fex::Status().state ==
                Executor::Fex::LoaderState::ActivationStranded,
            "configure quarantine was not absorbing");
    return 0;
  }
  if (std::strcmp(scenario, "preflight-reject") == 0) {
    Require(!preflight_ok, "incomplete adapter unexpectedly passed preflight");
    Require(Executor::Fex::Status().state == Executor::Fex::LoaderState::Failed,
            "preflight rejection did not remain fail-closed");
    Require(Executor::Fex::ActiveApi() == nullptr,
            "normal API leaked from rejected preflight");
    Require(Executor::Fex::ActiveApiForSignal() == nullptr,
            "signal API leaked from rejected preflight");
    const auto status = Executor::Fex::Status();
    std::printf("FEX_LOADER_LIFECYCLE PASS scenario=preflight-reject code=%d "
                "reason=%s\n",
                status.code, status.error.c_str());
    return 0;
  }
  Require(preflight_ok, "preflight failed");
  Require(Executor::Fex::Status().state ==
              Executor::Fex::LoaderState::Configured,
          "preflight did not stop at Configured");
  const char *const expected_namespace =
      std::strcmp(scenario, "relaxed-vector") == 0
          ? "fex-lifecycle-smoke-ab-vtso0-sm-swtso"
          : (std::strcmp(scenario, "relaxed-memcpy") == 0
                 ? "fex-lifecycle-smoke-ab-mtso0-sm-swtso"
                 : (std::strcmp(scenario, "relaxed-combined") == 0
                        ? "fex-lifecycle-smoke-ab-vm-tso0-sm-swtso"
                        : "fex-lifecycle-smoke-sm-swtso"));
  Require(Executor::Fex::Status().cache_namespace == expected_namespace,
          "software-TSO preflight did not isolate its cache namespace");
  if (std::strcmp(scenario, "ps5-profile") == 0) {
    Require(Executor::Fex::Status().platform_mask == LSX4_FEX_PLATFORM_PS5,
            "PS5 platform mask was not retained");
    Require(Executor::Fex::Status().guest_cpu_profile == "ps5-prospero-zen2-v2",
            "PS5 profile was not retained");
  }
  const bool relaxed = preflight.configure_flags != 0;
  Require(((Executor::Fex::Status().provided_capabilities &
            LSX4_FEX_CAP_STRONG_MEMORY) == 0) == relaxed,
          "configured capabilities misreported relaxed strong memory");
  if (relaxed) {
    Require((Executor::Fex::Status().provided_capabilities &
             LSX4_FEX_CAP_MEMORY_MODEL_AB_OVERRIDES) != 0,
            "relaxed-memory capability missing after configure");
  }
  Require(Executor::Fex::ActiveApi() == nullptr,
          "normal API leaked during preflight");
  Require(Executor::Fex::ActiveApiForSignal() == nullptr,
          "signal API leaked during preflight");

  if (std::strcmp(scenario, "preflight-mismatch") == 0) {
    Require(Executor::Fex::Preflight(preflight),
            "identical preflight was not idempotent");
    auto mismatched = preflight;
    mismatched.configure_flags = LSX4_FEX_CONFIG_RELAX_MEMCPY_SET_TSO_AB;
    Require(!Executor::Fex::Preflight(mismatched),
            "different immutable preflight request was accepted");
    const auto mismatch_status = Executor::Fex::Status();
    Require(mismatch_status.state == Executor::Fex::LoaderState::Configured,
            "request mismatch destroyed the configured runtime");
    Require(mismatch_status.error == "preflight-request-mismatch",
            "request mismatch reported the wrong reason");
    return 0;
  }

  if (std::strcmp(scenario, "commit-before-stage") == 0) {
    Require(!Executor::Fex::CommitActivation(),
            "commit unexpectedly succeeded before staging");
    Require(Executor::Fex::Status().state == Executor::Fex::LoaderState::Failed,
            "invalid early commit was not fail-closed");
    Require(Executor::Fex::ActiveApiForSignal() == nullptr,
            "signal API leaked after invalid early commit");
    return 0;
  }

  const bool staged = Executor::Fex::StageActivation(callbacks, nullptr);

  if (std::strcmp(scenario, "safe-failure") == 0) {
    Require(!staged, "injected safe failure unexpectedly staged");
    Require(Executor::Fex::Status().state == Executor::Fex::LoaderState::Failed,
            "safe failure was not marked fallback-safe");
    Require(Executor::Fex::ActiveApiForSignal() == nullptr,
            "signal API leaked after safe failure");
    return 0;
  }
  if (std::strcmp(scenario, "stranded") == 0) {
    Require(!staged, "invalid active contract unexpectedly staged");
    Require(Executor::Fex::Status().state ==
                Executor::Fex::LoaderState::ActivationStranded,
            "possibly active adapter was not quarantined");
    Require(Executor::Fex::ActiveApiForSignal() == nullptr,
            "signal API leaked from quarantined adapter");
    Require(!Executor::Fex::StageActivation(callbacks, nullptr),
            "repeat stage escaped activation quarantine");
    Require(!Executor::Fex::CommitActivation(),
            "commit escaped activation quarantine");
    Require(Executor::Fex::Status().state ==
                Executor::Fex::LoaderState::ActivationStranded,
            "activation quarantine was not absorbing");
    return 0;
  }

  Require(staged, "normal activation did not stage");
  const auto staged_status = Executor::Fex::Status();
  Require(staged_status.state == Executor::Fex::LoaderState::ActivationStaged,
          "adapter became visible before commit");
  Require(staged_status.activation_epoch == 1,
          "unexpected activation epoch after staging");
  Require(((staged_status.provided_capabilities & LSX4_FEX_CAP_STRONG_MEMORY) ==
           0) == relaxed,
          "activated capabilities misreported relaxed strong memory");
  Require(Executor::Fex::ActiveApi() == nullptr,
          "normal API leaked from staged activation");
  Require(Executor::Fex::ActiveApiForSignal() == nullptr,
          "signal API leaked from staged activation");
  Require(Executor::Fex::StageActivation(callbacks, nullptr),
          "idempotent stage failed");
  Require(Executor::Fex::Status().activation_epoch == 1,
          "idempotent stage called adapter twice");

  Require(Executor::Fex::CommitActivation(), "commit failed");
  Require(Executor::Fex::Status().state == Executor::Fex::LoaderState::Active,
          "commit did not publish Active state");
  Require(Executor::Fex::ActiveApi() != nullptr,
          "normal API missing after commit");
  Require(Executor::Fex::ActiveApiForSignal() != nullptr,
          "signal API missing after commit");
  Require(Executor::Fex::CommitActivation(), "idempotent commit failed");
  return 0;
}
