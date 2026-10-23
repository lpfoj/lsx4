// SPDX-FileCopyrightText: Copyright 2026 LSX4
// SPDX-License-Identifier: GPL-2.0-or-later

#include "executor/fex_backend_loader.h"

#include "executor/dynamic_translation/guest_capability_profile.h"
#include "executor/dynamic_translation/hle_thunk_identity.h"
#include "executor/dynamic_translation/host_isa_profile.h"

#include <dlfcn.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#include <limits.h>
#include <fcntl.h>
#include <sys/file.h>
#endif

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <utility>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace Executor::Fex {
namespace {

constexpr std::uint64_t kRequiredPlatformCapabilities =
    LSX4_FEX_CAP_MAPPED_ENTRY_V5 | LSX4_FEX_CAP_HLE_FRAME_V3 | LSX4_FEX_CAP_STRONG_MEMORY |
    LSX4_FEX_CAP_GUEST_SIGNALS | LSX4_FEX_CAP_GUEST_TLS | LSX4_FEX_CAP_GUEST_CALLBACKS |
    LSX4_FEX_CAP_GUEST_FUTEX_SCHEDULING | LSX4_FEX_CAP_SELF_MODIFYING_CODE |
    LSX4_FEX_CAP_EXECUTABLE_RANGE_V5 | LSX4_FEX_CAP_PROCESS_LIFETIME |
    LSX4_FEX_CAP_SINGLE_SIGNAL_OWNER | LSX4_FEX_CAP_EXACT_GUEST_CPU_PROFILE_V6;

constexpr std::uint32_t kKnownMemoryModelAbFlags =
    LSX4_FEX_CONFIG_RELAX_VECTOR_TSO_AB |
    LSX4_FEX_CONFIG_RELAX_MEMCPY_SET_TSO_AB;
constexpr std::uint32_t kKnownConfigureFlags =
    kKnownMemoryModelAbFlags | LSX4_FEX_CONFIG_EXACT_MULTIBLOCK_AB |
    LSX4_FEX_CONFIG_MAXINST_512_AB | LSX4_FEX_CONFIG_MAXINST_256_AB |
    LSX4_FEX_CONFIG_PERSISTENT_RIP_SEED_AB |
    LSX4_FEX_CONFIG_PRESERVE_TERMINAL_THUNK_DYNAMIC_REGS_AB |
    LSX4_FEX_CONFIG_PRESERVE_TYPED_TIME_COMMON_FRAME_AB |
    LSX4_FEX_CONFIG_PRESERVE_ZERO_DEFERRED_PROMOTION_RMW_AB;

constexpr std::uint32_t kRequiredLifetimeFlags =
    LSX4_FEX_LIFETIME_PROCESS_RESIDENT | LSX4_FEX_LIFETIME_ACTIVATE_ONCE |
    LSX4_FEX_LIFETIME_NO_DEACTIVATE | LSX4_FEX_LIFETIME_SINGLE_SIGNAL_OWNER;

struct State {
    std::mutex mutex;
    void* handle{};
    void* host_dso_pin{};
    int process_owner_socket{-1};
    Lsx4FexBackendApiV3 api{};
    LoaderStatus status{};
    PreflightRequest configured_request{};
    bool has_configured_request{};
    std::uint64_t activation_required_capabilities{kRequiredPlatformCapabilities};
    std::uint64_t activation_forbidden_capabilities{};
};

State& GlobalState() {
    static State state;
    return state;
}

bool ReserveProcessFexOwner(State& state) {
#if defined(LSX4_APPLE_FEX)
    if (state.process_owner_socket >= 0) return true;
    const char* temporary = std::getenv("TMPDIR");
    if (!temporary || temporary[0] != '/') return false;
    const std::string path = std::string(temporary) + "/lsx4-fex-owner-" + std::to_string(getpid());
    const int descriptor = open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (descriptor < 0) return false;
    struct stat metadata{};
    if (fstat(descriptor, &metadata) != 0 || !S_ISREG(metadata.st_mode) ||
        metadata.st_uid != getuid() || metadata.st_nlink != 1 ||
        flock(descriptor, LOCK_EX | LOCK_NB) != 0) {
        close(descriptor); return false;
    }
    // BSD flock is tied to this open-file description, so independently loaded
    // runtimes in this process cannot both own FEX. The kernel releases it on
    // process exit; an old temporary file cannot retain a stale lock.
    state.process_owner_socket = descriptor;
    return true;
#elif defined(__APPLE__)
    (void)state;
    return false;
#else
    if (state.process_owner_socket >= 0) {
        return true;
    }
    const int descriptor = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (descriptor < 0) {
        return false;
    }
    const std::string name = "lsx4-fex-owner-" + std::to_string(static_cast<long long>(getpid()));
    if (name.size() + 1 > sizeof(sockaddr_un::sun_path)) {
        close(descriptor);
        return false;
    }
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path + 1, name.data(), name.size());
    const auto address_size =
        static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + 1 + name.size());
    if (bind(descriptor, reinterpret_cast<const sockaddr*>(&address), address_size) != 0) {
        close(descriptor);
        return false;
    }
    state.process_owner_socket = descriptor;
    return true;
#endif
}

void ReleaseProcessFexOwner(State& state) {
    if (state.process_owner_socket >= 0) {
        close(state.process_owner_socket);
        state.process_owner_socket = -1;
    }
}

bool PinHostDsoForProcessLifetime(State& state) {
    if (state.host_dso_pin != nullptr) {
        return true;
    }
    Dl_info owner{};
    if (dladdr(reinterpret_cast<const void*>(&GlobalState), &owner) == 0 ||
        owner.dli_fname == nullptr || *owner.dli_fname == '\0') {
        return false;
    }
    state.host_dso_pin = dlopen(owner.dli_fname, RTLD_NOW | RTLD_LOCAL);
    if (state.host_dso_pin != nullptr) {
        return true;
    }
    // A lifecycle smoke may link the loader into the main executable. The
    // executable cannot be unloaded, so a failed self-dlopen is harmless in
    // that one case.
    struct stat owner_stat{};
    struct stat executable_stat{};
#if defined(__APPLE__)
    char executable_path[PATH_MAX]; uint32_t path_size=sizeof(executable_path);
    if (_NSGetExecutablePath(executable_path, &path_size) != 0) return false;
    const char* executable = executable_path;
#else
    const char* executable = "/proc/self/exe";
#endif
    return stat(owner.dli_fname, &owner_stat) == 0 &&
           stat(executable, &executable_stat) == 0 &&
           owner_stat.st_dev == executable_stat.st_dev &&
           owner_stat.st_ino == executable_stat.st_ino;
}

std::atomic<const Lsx4FexBackendApiV3*> g_signal_safe_active_api{};
using DecodeSignalContextGuestRipFn = std::uint64_t (*)(const void*) noexcept;
std::atomic<DecodeSignalContextGuestRipFn> g_decode_signal_context_guest_rip{};
using ExactStoreWatchConfigureFn = std::int32_t (*)(std::uint64_t, std::uint64_t,
                                                    std::uint32_t) noexcept;
using ExactStoreWatchTakeFn = std::int32_t (*)(ExactStoreWatchHit*) noexcept;
using GuestStoreTraceTakeFn = std::int32_t (*)(GuestStoreTraceHit*) noexcept;
using ProofMetricsQueryFn = std::int32_t (*)(Lsx4FexProofMetricsV1*) noexcept;
using DeferredWriteEpochConfigureFn = std::int32_t (*)(const DeferredWriteEpochCallbacks*) noexcept;
using DeferredWriteEpochArmCurrentFn = std::int32_t (*)(std::uint64_t) noexcept;
using DeferredWriteEpochKickOwnerFn = std::int32_t (*)(std::int64_t) noexcept;
std::atomic<ExactStoreWatchConfigureFn> g_exact_store_watch_configure{};
std::atomic<ExactStoreWatchTakeFn> g_exact_store_watch_take{};
std::atomic<GuestStoreTraceTakeFn> g_guest_store_trace_take{};
std::atomic<ProofMetricsQueryFn> g_proof_metrics_query{};
std::atomic<DeferredWriteEpochConfigureFn> g_deferred_write_epoch_configure{};
std::atomic<DeferredWriteEpochArmCurrentFn> g_deferred_write_epoch_arm_current{};
std::atomic<DeferredWriteEpochKickOwnerFn> g_deferred_write_epoch_kick_owner{};

std::string CopyString(const char* value) {
    return value != nullptr ? value : "";
}

void Fail(State& state, const std::int32_t code, const char* error) {
    // Once an adapter may have acquired process-global state there is no
    // deactivate contract.  Never downgrade the quarantine to an ordinary
    // failure, because callers are allowed to fall back to Current from the
    // latter state.
    if (state.status.state == LoaderState::ActivationStranded) {
        return;
    }
    state.status.state = LoaderState::Failed;
    state.status.code = code;
    state.status.error = CopyString(error);
}

void StrandActivation(State& state, const std::int32_t code, const char* error) {
    state.status.state = LoaderState::ActivationStranded;
    state.status.code = code;
    state.status.error = CopyString(error);
}

bool SamePreflightRequest(const PreflightRequest& lhs, const PreflightRequest& rhs) {
    return lhs.adapter_path == rhs.adapter_path && lhs.runtime_root == rhs.runtime_root &&
           lhs.cache_namespace == rhs.cache_namespace && lhs.host_build_id == rhs.host_build_id &&
           lhs.platform_mask == rhs.platform_mask && lhs.configure_flags == rhs.configure_flags &&
           lhs.cpu_tuning.identity == rhs.cpu_tuning.identity &&
           lhs.required_capabilities == rhs.required_capabilities &&
           lhs.host_isa_feature_mask == rhs.host_isa_feature_mask &&
           lhs.host_os_feature_mask == rhs.host_os_feature_mask &&
           lhs.max_sve_vector_bytes == rhs.max_sve_vector_bytes &&
           lhs.guest_cpu_profile == rhs.guest_cpu_profile;
}

bool HasRequiredFunctions(const Lsx4FexBackendApiV3& api) {
    return api.configure != nullptr && api.query != nullptr && api.activate != nullptr &&
           api.run_mapped_entry_v5 != nullptr && api.run_guest_callback != nullptr &&
           api.register_guest_signal_action != nullptr && api.queue_guest_signal != nullptr &&
           api.get_guest_context != nullptr && api.set_guest_context != nullptr &&
           api.get_guest_tls != nullptr && api.set_guest_tls != nullptr &&
           api.bind_host_thread != nullptr && api.unbind_host_thread != nullptr &&
           api.snapshot_executable_ranges_v5 != nullptr &&
           api.bulk_register_executable_ranges_v5 != nullptr &&
           api.invalidate_executable_ranges_v5 != nullptr && api.handle_host_signal != nullptr &&
           api.classify_signal_context_pc != nullptr && api.release_guest_thread != nullptr;
}

Lsx4FexCpuidResultV6 QueryFrozenPs4Cpuid(const void*, const std::uint32_t leaf,
                                         const std::uint32_t subleaf) {
    const auto value = Lsx4::Translation::QueryGuestCapabilityPage(leaf, subleaf);
    return {
        .eax = value.registers[0],
        .ebx = value.registers[1],
        .ecx = value.registers[2],
        .edx = value.registers[3],
    };
}

std::uint64_t QueryFrozenPs4Xcr(const void*, const std::uint32_t index) {
    return Lsx4::Translation::QueryGuestXcr(index);
}

const Lsx4FexGuestCpuProfileV6& FrozenPs4FexProfile() {
    static const Lsx4FexGuestCpuProfileV6 profile = [] {
        const auto& source = Lsx4::Translation::FrozenPs4GuestCpuProfile();
        return Lsx4FexGuestCpuProfileV6{
            .struct_size = sizeof(Lsx4FexGuestCpuProfileV6),
            .abi_version = LSX4_FEX_GUEST_CPU_PROFILE_ABI_V6,
            .identity_low = source.identity.low,
            .identity_high = source.identity.high,
            .profile_id = source.profile_id.data(),
            .profile_context = nullptr,
            .query_cpuid = QueryFrozenPs4Cpuid,
            .query_xcr = QueryFrozenPs4Xcr,
            .xcr0_writable_mask = source.xstate.xcr0_writable_mask,
            .mxcsr_reset_value = source.xstate.mxcsr_reset_value,
            .mxcsr_mask = source.xstate.mxcsr_mask,
            .legacy_save_area_bytes = source.xstate.legacy_save_area_bytes,
            .profile_flags = 0,
        };
    }();
    return profile;
}

Lsx4FexCpuidResultV6 QueryFrozenPs5Cpuid(const void*, const std::uint32_t leaf,
                                         const std::uint32_t subleaf) {
    // Stable Prospero/Zen 2 userspace profile. Keep cache leaves conservative:
    // games depend on the ISA leaves, while invented host cache geometry would
    // make persistent translations device-dependent. The legacy topology is a
    // coherent eight-core/eight-logical-processor package.
    switch (leaf) {
    case 0:
        return {
            .eax = 0x0000000d,
            .ebx = 0x68747541, // Auth
            .ecx = 0x444d4163, // cAMD
            .edx = 0x69746e65, // enti
        };
    case 1:
        return {
            .eax = 0x00870f10, // AMD family 17h, model 71h, stepping 0.
            .ebx = 0x00080800,
            .ecx = 0x7ed8320b,
            .edx = 0x178bfbff,
        };
    case 7:
        if (subleaf == 0) {
            return {
                .eax = 0,
                .ebx = 0x219c07a9,
                .ecx = 0x00400000,
                .edx = 0,
            };
        }
        return {};
    case 0x0d:
        if (subleaf == 0) {
            return {.eax = 7, .ebx = 832, .ecx = 832, .edx = 0};
        }
        if (subleaf == 1) {
            return {.eax = 1, .ebx = 0, .ecx = 0, .edx = 0};
        }
        if (subleaf == 2) {
            return {.eax = 256, .ebx = 576, .ecx = 0, .edx = 0};
        }
        return {};
    case 0x80000000:
        return {.eax = 0x80000008, .ebx = 0, .ecx = 0, .edx = 0};
    case 0x80000001:
        return {
            .eax = 0x00870f10,
            .ebx = 0,
            .ecx = 0x01ebbfff,
            .edx = 0x2fd3fbff,
        };
    case 0x80000007:
        return {.eax = 0, .ebx = 0, .ecx = 0, .edx = 1u << 8};
    case 0x80000008:
        return {
            .eax = 0x00003030,
            .ebx = 0x00000209,
            .ecx = 7, // NC: eight physical cores minus one.
            .edx = 0,
        };
    default:
        return {};
    }
}

std::uint64_t QueryFrozenPs5Xcr(const void*, const std::uint32_t index) {
    return index == 0 ? UINT64_C(7) : 0;
}

const Lsx4FexGuestCpuProfileV6& FrozenPs5FexProfile() {
    static constexpr char ProfileId[] = "ps5-prospero-zen2-v2";
    static const Lsx4FexGuestCpuProfileV6 profile{
        .struct_size = sizeof(Lsx4FexGuestCpuProfileV6),
        .abi_version = LSX4_FEX_GUEST_CPU_PROFILE_ABI_V6,
        .identity_low = UINT64_C(0xca978c3024fac2f4),
        .identity_high = UINT64_C(0x2e736ab11e7831be),
        .profile_id = ProfileId,
        .profile_context = nullptr,
        .query_cpuid = QueryFrozenPs5Cpuid,
        .query_xcr = QueryFrozenPs5Xcr,
        .xcr0_writable_mask = UINT64_C(7),
        .mxcsr_reset_value = UINT32_C(0x1f80),
        .mxcsr_mask = UINT32_C(0x0000ffff),
        .legacy_save_area_bytes = 512,
        .profile_flags = 0,
    };
    return profile;
}

const Lsx4FexGuestCpuProfileV6* ResolveGuestCpuProfile(const PreflightRequest& request) {
    GuestCpuProfileKind kind = request.guest_cpu_profile;
    if (kind == GuestCpuProfileKind::PlatformDefault) {
        if (request.platform_mask == LSX4_FEX_PLATFORM_PS4) {
            kind = GuestCpuProfileKind::Ps4Jaguar;
        } else if (request.platform_mask == LSX4_FEX_PLATFORM_PS5) {
            kind = GuestCpuProfileKind::Ps5Zen2;
        } else {
            return nullptr;
        }
    }
    if (kind == GuestCpuProfileKind::Ps4Jaguar && request.platform_mask == LSX4_FEX_PLATFORM_PS4) {
        return &FrozenPs4FexProfile();
    }
    if (kind == GuestCpuProfileKind::Ps5Zen2 && request.platform_mask == LSX4_FEX_PLATFORM_PS5) {
        return &FrozenPs5FexProfile();
    }
    return nullptr;
}

} // namespace

std::string BuildPlatformCacheNamespace(const std::string_view adapter_family,
                                        const std::uint32_t platform_mask,
                                        const GuestCpuProfileKind guest_cpu_profile,
                                        const std::uint32_t namespace_version,
                                        const std::uint64_t host_isa_feature_mask,
                                        const std::uint32_t sve_vector_bytes,
                                        const std::string_view policy_suffix) {
    PreflightRequest profile_request{};
    profile_request.platform_mask = platform_mask;
    profile_request.guest_cpu_profile = guest_cpu_profile;
    const auto* const profile = ResolveGuestCpuProfile(profile_request);
    const char* platform_name = nullptr;
    if (platform_mask == LSX4_FEX_PLATFORM_PS4) {
        platform_name = "ps4";
    } else if (platform_mask == LSX4_FEX_PLATFORM_PS5) {
        platform_name = "ps5";
    }
    if (adapter_family.empty() || platform_name == nullptr || profile == nullptr ||
        profile->profile_id == nullptr || namespace_version == 0) {
        return {};
    }
    std::ostringstream output;
    output << adapter_family << '-' << platform_name << "-v" << namespace_version << "-cpu-"
           << std::hex << std::setfill('0') << std::setw(16) << profile->identity_low << '-'
           << std::setw(16) << profile->identity_high << "-isa" << std::setw(16)
           << host_isa_feature_mask << std::dec << "-vl" << sve_vector_bytes << policy_suffix;
    return output.str();
}

bool Preflight(const PreflightRequest& request) noexcept {
    State& state = GlobalState();
    std::lock_guard lock(state.mutex);
    if (state.status.state == LoaderState::Configured ||
        state.status.state == LoaderState::ActivationStaged ||
        state.status.state == LoaderState::Active) {
        if (state.has_configured_request &&
            SamePreflightRequest(state.configured_request, request)) {
            return true;
        }
        // The adapter contract is process-immutable after configure(). Keep
        // an already active runtime operational, but never let a caller
        // relabel it with different markers, masks, or cache provenance.
        state.status.code = LSX4_FEX_STATUS_INVALID_STATE;
        state.status.error = "preflight-request-mismatch";
        return false;
    }
    if (state.status.state == LoaderState::Failed ||
        state.status.state == LoaderState::ActivationStranded) {
        return false;
    }
    if (request.adapter_path.empty() || request.runtime_root.empty() ||
        request.cache_namespace.empty()) {
        Fail(state, LSX4_FEX_STATUS_INVALID_ARGUMENT, "invalid-preflight-request");
        return false;
    }
    const Lsx4FexGuestCpuProfileV6* const guest_cpu_profile = ResolveGuestCpuProfile(request);
    if (guest_cpu_profile == nullptr) {
        Fail(state, LSX4_FEX_STATUS_INVALID_ARGUMENT, "platform-profile-mismatch");
        return false;
    }
    if (request.cpu_tuning.struct_size != sizeof(Lsx4FexCpuTuningV1) ||
        request.cpu_tuning.abi_version != LSX4_FEX_CPU_TUNING_ABI_V1 ||
        request.cpu_tuning.identity == 0) {
        Fail(state, LSX4_FEX_STATUS_INVALID_ARGUMENT, "invalid-cpu-tuning-v1");
        return false;
    }
    const bool relax_vector = (request.configure_flags & LSX4_FEX_CONFIG_RELAX_VECTOR_TSO_AB) != 0;
    const bool relax_memcpy =
        (request.configure_flags & LSX4_FEX_CONFIG_RELAX_MEMCPY_SET_TSO_AB) != 0;
    const bool relaxed_memory_ab = relax_vector || relax_memcpy;
    if ((request.configure_flags & ~kKnownConfigureFlags) != 0) {
        Fail(state, LSX4_FEX_STATUS_INVALID_ARGUMENT, "invalid-configure-flags");
        return false;
    }
    const bool maxinst_512 =
        (request.configure_flags & LSX4_FEX_CONFIG_MAXINST_512_AB) != 0;
    const bool maxinst_256 =
        (request.configure_flags & LSX4_FEX_CONFIG_MAXINST_256_AB) != 0;
    if (maxinst_512 && maxinst_256) {
        Fail(state, LSX4_FEX_STATUS_INVALID_ARGUMENT,
             "conflicting-maxinst-geometry-flags");
        return false;
    }

    // Caller requirements are additive. They may strengthen the platform
    // contract, but a partial mask must never drop signal, SMC, TLS,
    // lifetime, or exact guest-CPU-profile requirements.
    const bool explicit_strong = (request.required_capabilities & LSX4_FEX_CAP_STRONG_MEMORY) != 0;
    if (relaxed_memory_ab && explicit_strong) {
        Fail(state, LSX4_FEX_STATUS_INVALID_ARGUMENT,
             "relaxed-memory-ab-conflicts-with-explicit-strong-memory");
        return false;
    }
    std::uint64_t required = kRequiredPlatformCapabilities | request.required_capabilities;
    if (relaxed_memory_ab) {
        required &= ~LSX4_FEX_CAP_STRONG_MEMORY;
        required |= LSX4_FEX_CAP_MEMORY_MODEL_AB_OVERRIDES;
    }
    if ((request.platform_mask & (LSX4_FEX_PLATFORM_PS4 | LSX4_FEX_PLATFORM_PS5)) != 0 &&
        (required & LSX4_FEX_CAP_EXACT_GUEST_CPU_PROFILE_V6) == 0) {
        Fail(state, LSX4_FEX_STATUS_MISSING_CAPABILITY,
             "platform-preflight-must-require-exact-guest-cpu-profile-v6");
        return false;
    }

#if defined(LSX4_APPLE_FEX)
    state.handle = dlopen(request.adapter_path == "@apple-embedded-fex-v3" ? nullptr : request.adapter_path.c_str(), RTLD_NOW | RTLD_LOCAL);
#else
    state.handle = dlopen(request.adapter_path.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
    if (state.handle == nullptr) {
        Fail(state, LSX4_FEX_STATUS_NOT_FOUND, dlerror());
        return false;
    }
    g_decode_signal_context_guest_rip.store(
        reinterpret_cast<DecodeSignalContextGuestRipFn>(
            dlsym(state.handle, "lsx4_fex_backend_decode_signal_context_guest_rip_v1")),
        std::memory_order_release);
    g_exact_store_watch_configure.store(
        reinterpret_cast<ExactStoreWatchConfigureFn>(
            dlsym(state.handle, "lsx4_fex_backend_exact_store_watch_configure_v1")),
        std::memory_order_release);
    g_exact_store_watch_take.store(reinterpret_cast<ExactStoreWatchTakeFn>(dlsym(
                                       state.handle, "lsx4_fex_backend_exact_store_watch_take_v1")),
                                   std::memory_order_release);
    g_guest_store_trace_take.store(reinterpret_cast<GuestStoreTraceTakeFn>(dlsym(
                                       state.handle, "lsx4_fex_backend_guest_store_trace_take_v1")),
                                   std::memory_order_release);
    g_proof_metrics_query.store(
        reinterpret_cast<ProofMetricsQueryFn>(
            dlsym(state.handle, LSX4_FEX_BACKEND_PROOF_METRICS_QUERY_V1_SYMBOL)),
        std::memory_order_release);
    g_deferred_write_epoch_configure.store(
        reinterpret_cast<DeferredWriteEpochConfigureFn>(
            dlsym(state.handle, "lsx4_fex_backend_deferred_write_epoch_configure_v1")),
        std::memory_order_release);
    g_deferred_write_epoch_arm_current.store(
        reinterpret_cast<DeferredWriteEpochArmCurrentFn>(
            dlsym(state.handle, "lsx4_fex_backend_deferred_write_epoch_arm_current_v1")),
        std::memory_order_release);
    g_deferred_write_epoch_kick_owner.store(
        reinterpret_cast<DeferredWriteEpochKickOwnerFn>(
            dlsym(state.handle, "lsx4_fex_backend_deferred_write_epoch_kick_owner_v1")),
        std::memory_order_release);
    const auto get_api = reinterpret_cast<Lsx4FexGetBackendApiV3Fn>(
        dlsym(state.handle, LSX4_FEX_BACKEND_GET_API_V3_SYMBOL));
    if (get_api == nullptr) {
        Fail(state, LSX4_FEX_STATUS_ABI_MISMATCH, "get-api-v3-missing");
        return false;
    }

    state.api = {};
    state.api.struct_size = sizeof(state.api);
    state.api.abi_version = LSX4_FEX_BACKEND_ABI_V3;
    const std::int32_t get_api_status =
        get_api(LSX4_FEX_BACKEND_ABI_V3, sizeof(state.api), &state.api);
    if (get_api_status != LSX4_FEX_STATUS_OK ||
        state.api.struct_size < sizeof(Lsx4FexBackendApiV3) ||
        state.api.abi_version != LSX4_FEX_BACKEND_ABI_V3) {
        Fail(state,
             get_api_status != LSX4_FEX_STATUS_OK ? get_api_status : LSX4_FEX_STATUS_ABI_MISMATCH,
             "get-api-v3-invalid-table");
        return false;
    }

    if ((state.api.provided_capabilities & required) != required ||
        (state.api.lifetime_flags & kRequiredLifetimeFlags) != kRequiredLifetimeFlags ||
        !HasRequiredFunctions(state.api)) {
        Fail(state, LSX4_FEX_STATUS_MISSING_CAPABILITY, "api-table-missing-required-contract");
        return false;
    }

    const auto& host = Jit::GetHostIsaProfile();
    constexpr std::uint64_t kHardwareTso = LSX4_FEX_HOST_OS_HARDWARE_TSO_ENABLED;
    std::uint64_t effective_host_os_features = host.os_features & request.host_os_feature_mask;
    // The cached ISA profile may have been sampled on a different thread.
    // Never advertise hardware TSO from that cached observation alone.
    effective_host_os_features &= ~kHardwareTso;
    if ((request.host_os_feature_mask & kHardwareTso) != 0) {
        const Jit::HardwareTsoAttempt tso_attempt = Jit::TryEnableHardwareTsoForCurrentThread();
        if (tso_attempt.enabled) {
            effective_host_os_features |= kHardwareTso;
        }
        // Preflight establishes capability, but this host thread doesn't run
        // guest code.  Avoid making unrelated children inherit TSO.
        Jit::RestoreHardwareTsoAfterProbe(tso_attempt);
    }
    const bool hardware_tso = (effective_host_os_features & kHardwareTso) != 0;
    if (relaxed_memory_ab && hardware_tso) {
        Fail(state, LSX4_FEX_STATUS_INVALID_ARGUMENT, "relaxed-memory-ab-requires-software-tso");
        return false;
    }
    const char* const memory_model_suffix =
        relax_vector && relax_memcpy
            ? "-ab-vm-tso0"
            : (relax_vector ? "-ab-vtso0" : (relax_memcpy ? "-ab-mtso0" : ""));
    const char* const multiblock_suffix =
        (request.configure_flags & LSX4_FEX_CONFIG_EXACT_MULTIBLOCK_AB) != 0
            ? "-ab-mb1"
            : "";
    const char* const maxinst_suffix =
        maxinst_256 ? "-ab-mi256" : (maxinst_512 ? "-ab-mi512" : "");
    const char* const persistent_rip_seed_suffix =
        (request.configure_flags & LSX4_FEX_CONFIG_PERSISTENT_RIP_SEED_AB) != 0
            ? "-ab-ripseed1"
            : "";
    std::ostringstream cpu_policy_suffix;
    cpu_policy_suffix << "-cpu-" << std::hex << std::setw(16)
                      << std::setfill('0') << request.cpu_tuning.identity;
    const std::string effective_cache_namespace =
        request.cache_namespace + memory_model_suffix + multiblock_suffix + maxinst_suffix +
        persistent_rip_seed_suffix + cpu_policy_suffix.str() +
        (hardware_tso ? "-sm-hwtso" : "-sm-swtso");
    Lsx4FexConfigureRequestV3 configure{};
    configure.struct_size = sizeof(configure);
    configure.abi_version = LSX4_FEX_BACKEND_ABI_V3;
    configure.platform_mask = request.platform_mask;
    configure.configure_flags = request.configure_flags;
    configure.runtime_root = request.runtime_root.c_str();
    configure.cache_namespace = effective_cache_namespace.c_str();
    configure.hle_thunk_identity = Lsx4::Translation::FexThunkIdentity.data();
    configure.hle_thunk_identity_size = Lsx4::Translation::FexThunkIdentity.size();
    configure.required_capabilities = required;
    configure.host_build_id = request.host_build_id.c_str();
    configure.request_cookie = UINT64_C(0x4c53583446455833);
    configure.host_hwcap = host.hwcap;
    configure.host_hwcap2 = host.hwcap2;
    configure.host_isa_features = host.features & request.host_isa_feature_mask;
    configure.host_os_features = effective_host_os_features;
    configure.sve_vector_bytes = std::min(host.sve_vector_bytes, request.max_sve_vector_bytes);
    constexpr std::uint64_t kSveFeatures = LSX4_FEX_HOST_ISA_SVE | LSX4_FEX_HOST_ISA_SVE2;
    if ((configure.host_isa_features & kSveFeatures) != kSveFeatures ||
        (configure.host_os_features & LSX4_FEX_HOST_OS_SVE_CONTEXT) == 0) {
        configure.host_isa_features &= ~kSveFeatures;
        configure.host_os_features &= ~LSX4_FEX_HOST_OS_SVE_CONTEXT;
        configure.sve_vector_bytes = 0;
    }
    configure.guest_cpu_profile_v6 = guest_cpu_profile;
    configure.cpu_tuning_v1 = &request.cpu_tuning;

    if (!ReserveProcessFexOwner(state)) {
        Fail(state, LSX4_FEX_STATUS_INVALID_STATE, "process-fex-owner-already-reserved");
        return false;
    }

    Lsx4FexConfigureResultV3 result{};
    result.struct_size = sizeof(result);
    result.abi_version = LSX4_FEX_BACKEND_ABI_V3;
    result.request_cookie = configure.request_cookie;
    const std::int32_t configure_status =
        state.api.configure(state.api.backend_context, &configure, &result);
    if (configure_status != LSX4_FEX_STATUS_OK || result.status != LSX4_FEX_STATUS_OK ||
        result.lifecycle_state != LSX4_FEX_LIFECYCLE_CONFIGURED ||
        result.mapped_entry_abi != LSX4_FEX_MAPPED_ENTRY_ABI_V5 ||
        result.executable_range_abi != LSX4_FEX_EXECUTABLE_RANGE_ABI_V5 ||
        result.request_cookie != configure.request_cookie ||
        (result.provided_capabilities & required) != required ||
        (relaxed_memory_ab && (result.provided_capabilities & LSX4_FEX_CAP_STRONG_MEMORY) != 0)) {
        const std::int32_t failure_code =
            configure_status != LSX4_FEX_STATUS_OK
                ? configure_status
                : (result.status != LSX4_FEX_STATUS_OK ? result.status
                                                       : LSX4_FEX_STATUS_ABI_MISMATCH);
        const char* const failure_message =
            result.error_message != nullptr ? result.error_message : "configure-contract-failed";
        // configure() may not acquire process-global state. If a malformed
        // adapter nevertheless claims ACTIVE, quarantining the process is the
        // only safe alternative to starting Current beside it.
        if (result.lifecycle_state == LSX4_FEX_LIFECYCLE_ACTIVE) {
            StrandActivation(state, failure_code, failure_message);
        } else {
            ReleaseProcessFexOwner(state);
            Fail(state, failure_code, failure_message);
        }
        return false;
    }

    state.status.state = LoaderState::Configured;
    state.status.code = LSX4_FEX_STATUS_OK;
    state.status.configure_flags = request.configure_flags;
    state.status.provided_capabilities = result.provided_capabilities;
    state.status.required_capabilities = required;
    state.status.forbidden_capabilities = relaxed_memory_ab ? LSX4_FEX_CAP_STRONG_MEMORY : 0;
    state.status.platform_mask = request.platform_mask;
    state.status.adapter_name = CopyString(result.adapter_name);
    state.status.adapter_version = CopyString(result.adapter_version);
    state.status.guest_cpu_profile = guest_cpu_profile->profile_id;
    state.status.strong_memory_model = CopyString(result.strong_memory_model);
    state.status.cache_namespace = effective_cache_namespace;
    state.status.error.clear();
    state.configured_request = request;
    state.has_configured_request = true;
    state.activation_required_capabilities = required;
    state.activation_forbidden_capabilities = relaxed_memory_ab ? LSX4_FEX_CAP_STRONG_MEMORY : 0;
    return true;
}

bool StageActivation(const Lsx4FexHostCallbacksV3& callbacks,
                     const Lsx4FexExecutableRangeSnapshotV5* initial_ranges) noexcept {
    State& state = GlobalState();
    std::lock_guard lock(state.mutex);
    if (state.status.state == LoaderState::ActivationStaged ||
        state.status.state == LoaderState::Active) {
        return true;
    }
    if (state.status.state == LoaderState::ActivationStranded) {
        return false;
    }
    if (state.status.state != LoaderState::Configured ||
        callbacks.struct_size < sizeof(callbacks) ||
        callbacks.abi_version != LSX4_FEX_BACKEND_ABI_V3 || callbacks.dispatch_hle == nullptr ||
        callbacks.fatal_error == nullptr) {
        Fail(state, LSX4_FEX_STATUS_INVALID_STATE, "activate-precondition-failed");
        return false;
    }
    if (!PinHostDsoForProcessLifetime(state)) {
        ReleaseProcessFexOwner(state);
        Fail(state, LSX4_FEX_STATUS_BACKEND_FAILURE, "host-dso-process-pin-failed");
        return false;
    }

    Lsx4FexActivateRequestV3 request{};
    request.struct_size = sizeof(request);
    request.abi_version = LSX4_FEX_BACKEND_ABI_V3;
    request.host_callbacks = &callbacks;
    request.initial_executable_ranges = initial_ranges;
    request.hle_thunk_identity = Lsx4::Translation::FexThunkIdentity.data();
    request.hle_thunk_identity_size = Lsx4::Translation::FexThunkIdentity.size();
    request.request_cookie = UINT64_C(0x4c53583441435433);

    Lsx4FexActivateResultV3 result{};
    result.struct_size = sizeof(result);
    result.abi_version = LSX4_FEX_BACKEND_ABI_V3;
    result.request_cookie = request.request_cookie;
    const std::int32_t activate_status =
        state.api.activate(state.api.backend_context, &request, &result);
    if (activate_status != LSX4_FEX_STATUS_OK || result.status != LSX4_FEX_STATUS_OK ||
        result.lifecycle_state != LSX4_FEX_LIFECYCLE_ACTIVE ||
        result.request_cookie != request.request_cookie ||
        (result.effective_capabilities & state.activation_required_capabilities) !=
            state.activation_required_capabilities ||
        (result.effective_capabilities & state.activation_forbidden_capabilities) != 0) {
        const std::int32_t failure_code =
            activate_status != LSX4_FEX_STATUS_OK
                ? activate_status
                : (result.status != LSX4_FEX_STATUS_OK ? result.status
                                                       : LSX4_FEX_STATUS_ABI_MISMATCH);
        const char* const failure_message =
            result.error_message != nullptr ? result.error_message : "activate-contract-failed";
        // An adapter that returned success, or claimed ACTIVE in its result,
        // may already own process-global state and has no deactivate contract.
        // Keep that process quarantined instead of silently falling back to
        // Current with two runtimes potentially observing the same signals.
        if (activate_status == LSX4_FEX_STATUS_OK ||
            result.lifecycle_state == LSX4_FEX_LIFECYCLE_ACTIVE) {
            StrandActivation(state, failure_code, failure_message);
        } else {
            ReleaseProcessFexOwner(state);
            Fail(state, failure_code, failure_message);
        }
        return false;
    }

    state.status.state = LoaderState::ActivationStaged;
    state.status.code = LSX4_FEX_STATUS_OK;
    state.status.activation_epoch = result.activation_epoch;
    state.status.provided_capabilities = result.effective_capabilities;
    state.status.error.clear();
    return true;
}

bool CommitActivation() noexcept {
    State& state = GlobalState();
    std::lock_guard lock(state.mutex);
    if (state.status.state == LoaderState::Active) {
        return true;
    }
    if (state.status.state == LoaderState::ActivationStranded) {
        return false;
    }
    if (state.status.state != LoaderState::ActivationStaged) {
        Fail(state, LSX4_FEX_STATUS_INVALID_STATE, "activation-commit-precondition-failed");
        return false;
    }

    // Publish the process-resident table before the host commits FEX as the
    // immutable fault-signal owner. Signal dispatch requires both records, so
    // this intermediate state is deliberately unreachable by signal handlers.
    g_signal_safe_active_api.store(&state.api, std::memory_order_release);
    state.status.state = LoaderState::Active;
    return true;
}

LoaderStatus Status() {
    State& state = GlobalState();
    std::lock_guard lock(state.mutex);
    return state.status;
}

const Lsx4FexBackendApiV3* ActiveApi() noexcept {
    State& state = GlobalState();
    std::lock_guard lock(state.mutex);
    return state.status.state == LoaderState::Active ? &state.api : nullptr;
}

const Lsx4FexBackendApiV3* ActiveApiForSignal() noexcept {
    return g_signal_safe_active_api.load(std::memory_order_acquire);
}

std::uint64_t DecodeSignalContextGuestRip(const void* const raw_context) noexcept {
    if (g_signal_safe_active_api.load(std::memory_order_acquire) == nullptr) {
        return 0;
    }
    const auto decode = g_decode_signal_context_guest_rip.load(std::memory_order_acquire);
    return decode != nullptr ? decode(raw_context) : 0;
}

bool ConfigureExactStoreWatch(const std::uint64_t begin, const std::uint64_t end,
                              const std::uint32_t flags) noexcept {
    if (g_signal_safe_active_api.load(std::memory_order_acquire) == nullptr) {
        return false;
    }
    const auto configure = g_exact_store_watch_configure.load(std::memory_order_acquire);
    return configure != nullptr && configure(begin, end, flags) == 0;
}

int TakeExactStoreWatchHit(ExactStoreWatchHit& hit) noexcept {
    if (g_signal_safe_active_api.load(std::memory_order_acquire) == nullptr) {
        return -1;
    }
    const auto take = g_exact_store_watch_take.load(std::memory_order_acquire);
    if (take == nullptr) {
        return -1;
    }
    hit = ExactStoreWatchHit{};
    return take(&hit);
}

int TakeGuestStoreTraceHit(GuestStoreTraceHit& hit) noexcept {
    if (g_signal_safe_active_api.load(std::memory_order_acquire) == nullptr) {
        return -1;
    }
    const auto take = g_guest_store_trace_take.load(std::memory_order_acquire);
    if (take == nullptr) {
        return -1;
    }
    hit = GuestStoreTraceHit{};
    return take(&hit);
}

int QueryProofMetrics(Lsx4FexProofMetricsV1& metrics) noexcept {
    if (g_signal_safe_active_api.load(std::memory_order_acquire) == nullptr) {
        return LSX4_FEX_STATUS_INVALID_STATE;
    }
    const auto query = g_proof_metrics_query.load(std::memory_order_acquire);
    if (query == nullptr) {
        return LSX4_FEX_STATUS_NOT_FOUND;
    }
    metrics = Lsx4FexProofMetricsV1{};
    metrics.struct_size = sizeof(metrics);
    metrics.abi_version = LSX4_FEX_PROOF_METRICS_ABI_V1;
    return query(&metrics);
}

bool ConfigureDeferredWriteEpoch(const DeferredWriteEpochCallbacks& callbacks) noexcept {
    State& state = GlobalState();
    std::lock_guard lock(state.mutex);
    if (state.status.state != LoaderState::ActivationStaged ||
        callbacks.struct_size < sizeof(callbacks) ||
        callbacks.abi_version != DeferredWriteEpochAbiV1 || callbacks.publish_ready == nullptr) {
        return false;
    }
    const auto configure = g_deferred_write_epoch_configure.load(std::memory_order_acquire);
    return configure != nullptr && configure(&callbacks) == LSX4_FEX_STATUS_OK;
}

int ArmDeferredWriteEpochCurrent(const std::uint64_t token) noexcept {
    if (g_signal_safe_active_api.load(std::memory_order_acquire) == nullptr) {
        return 0;
    }
    const auto arm = g_deferred_write_epoch_arm_current.load(std::memory_order_acquire);
    return arm != nullptr ? arm(token) : 0;
}

int KickDeferredWriteEpochOwner(const std::int64_t host_tid) noexcept {
    if (host_tid <= 0 || g_signal_safe_active_api.load(std::memory_order_acquire) == nullptr) {
        return LSX4_FEX_STATUS_INVALID_STATE;
    }
    const auto kick = g_deferred_write_epoch_kick_owner.load(std::memory_order_acquire);
    return kick != nullptr ? kick(host_tid) : LSX4_FEX_STATUS_NOT_FOUND;
}

} // namespace Executor::Fex
