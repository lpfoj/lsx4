// SPDX-FileCopyrightText: Copyright 2026 LSX4
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "executor/fex_backend_abi_v3.h"
#include "executor/fex_proof_metrics_abi_v1.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace Executor::Fex {

enum class LoaderState : std::uint32_t {
    Empty = 0,
    Configured = 1,
    ActivationStaged = 2,
    Active = 3,
    Failed = 4,

    ActivationStranded = 5,
};

enum class GuestCpuProfileKind : std::uint32_t {
    PlatformDefault = 0,
    Ps4Jaguar = 1,
    Ps5Zen2 = 2,
};

struct PreflightRequest {
    std::string adapter_path;
    std::string runtime_root;
    std::string cache_namespace;
    std::string host_build_id;
    std::uint32_t platform_mask{LSX4_FEX_PLATFORM_PS4};
    std::uint32_t configure_flags{};
    std::uint64_t required_capabilities{};
    std::uint64_t host_isa_feature_mask{~std::uint64_t{}};
    std::uint64_t host_os_feature_mask{~std::uint64_t{}};
    std::uint32_t max_sve_vector_bytes{~std::uint32_t{}};
    GuestCpuProfileKind guest_cpu_profile{GuestCpuProfileKind::PlatformDefault};
    Lsx4FexCpuTuningV1 cpu_tuning{};
};

struct LoaderStatus {
    LoaderState state{LoaderState::Empty};
    std::int32_t code{};
    std::uint32_t configure_flags{};
    std::uint64_t provided_capabilities{};
    std::uint64_t required_capabilities{};
    std::uint64_t forbidden_capabilities{};
    std::uint64_t activation_epoch{};
    std::uint32_t platform_mask{};
    std::string adapter_name;
    std::string adapter_version;
    std::string guest_cpu_profile;
    std::string strong_memory_model;
    std::string cache_namespace;
    std::string error;
};






[[nodiscard]] std::string BuildPlatformCacheNamespace(std::string_view adapter_family,
                                                      std::uint32_t platform_mask,
                                                      GuestCpuProfileKind guest_cpu_profile,
                                                      std::uint32_t namespace_version,
                                                      std::uint64_t host_isa_feature_mask,
                                                      std::uint32_t sve_vector_bytes,
                                                      std::string_view policy_suffix = {});

enum ExactStoreWatchFlags : std::uint32_t {
    ExactStoreWatchEnableInstrumentation = 1U << 0,
    ExactStoreWatchArmRange = 1U << 1,
    ExactStoreWatchClearHit = 1U << 2,
};

struct ExactStoreWatchHit {
    std::uint32_t struct_size{sizeof(ExactStoreWatchHit)};
    std::uint32_t sequence{};
    std::uint64_t guest_rip{};
    std::uint64_t effective_address{};
    std::uint32_t access_size{};
    std::uint32_t reserved{};
};

struct GuestStoreTraceHit {
    std::uint32_t struct_size{sizeof(GuestStoreTraceHit)};
    std::uint32_t flags{};
    std::uint64_t sequence{};
    std::uint64_t host_tid{};
    std::uint64_t guest_rip{};
    std::uint64_t block_entry_rip{};
    std::uint64_t address_base{};
    std::uint64_t address_offset{};
    std::uint64_t gpr_rax{};
    std::uint64_t gpr_rbx{};
    std::uint64_t gpr_r8{};
    std::uint64_t gpr_r9{};
    std::uint64_t gpr_rsi{};
    std::uint64_t gpr_rdi{};
    std::uint64_t gpr_r14{};
    std::uint64_t gpr_r15{};
    std::uint64_t gpr_rcx{};
    std::uint64_t gpr_rdx{};
    std::uint64_t gpr_r10{};
    std::uint64_t gpr_r11{};
    std::uint64_t gpr_r12{};
    std::uint64_t gpr_r13{};
    std::uint64_t value[4]{};
    std::uint64_t value2[4]{};
    std::uint64_t xmm0[2]{};
    std::uint64_t xmm1[2]{};
    std::uint64_t xmm2[2]{};
    std::uint64_t xmm3[2]{};
    std::uint64_t xmm4[2]{};
    std::uint64_t xmm8[2]{};
    std::uint64_t xmm9[2]{};
    std::uint64_t xmm10[2]{};
    std::uint64_t xmm11[2]{};
    std::uint32_t access_size{};
    std::uint32_t store_class{};
    std::uint32_t offset_type{};
    std::uint32_t offset_scale{};
};

inline constexpr std::uint32_t DeferredWriteEpochAbiV1 = 1;
using DeferredWriteEpochReadyFn = void (*)(void* callback_context, std::uint64_t token) noexcept;

struct DeferredWriteEpochCallbacks {
    std::uint32_t struct_size{sizeof(DeferredWriteEpochCallbacks)};
    std::uint32_t abi_version{DeferredWriteEpochAbiV1};
    void* callback_context{};
    DeferredWriteEpochReadyFn publish_ready{};
};

[[nodiscard]] bool Preflight(const PreflightRequest& request) noexcept;





[[nodiscard]] bool StageActivation(const Lsx4FexHostCallbacksV3& callbacks,
                                   const Lsx4FexExecutableRangeSnapshotV5* initial_ranges) noexcept;

[[nodiscard]] bool CommitActivation() noexcept;
[[nodiscard]] LoaderStatus Status();
[[nodiscard]] const Lsx4FexBackendApiV3* ActiveApi() noexcept;

[[nodiscard]] const Lsx4FexBackendApiV3* ActiveApiForSignal() noexcept;

[[nodiscard]] std::uint64_t DecodeSignalContextGuestRip(const void* raw_context) noexcept;

[[nodiscard]] bool ConfigureExactStoreWatch(std::uint64_t begin, std::uint64_t end,
                                            std::uint32_t flags) noexcept;

[[nodiscard]] int TakeExactStoreWatchHit(ExactStoreWatchHit& hit) noexcept;

[[nodiscard]] int TakeGuestStoreTraceHit(GuestStoreTraceHit& hit) noexcept;





[[nodiscard]] int QueryProofMetrics(Lsx4FexProofMetricsV1& metrics) noexcept;


[[nodiscard]] bool ConfigureDeferredWriteEpoch(
    const DeferredWriteEpochCallbacks& callbacks) noexcept;
[[nodiscard]] int ArmDeferredWriteEpochCurrent(std::uint64_t token) noexcept;
[[nodiscard]] int KickDeferredWriteEpochOwner(std::int64_t host_tid) noexcept;

}
