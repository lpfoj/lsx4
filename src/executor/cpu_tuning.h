// SPDX-FileCopyrightText: Copyright 2026 LSX4
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <system_error>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <unordered_map>

namespace Executor::CpuTuning {



enum class Preset : std::uint32_t { Compatibility, Balanced, Performance, Extreme, Custom };
enum class ExecutionMode : std::uint32_t { Jit, IrJit, CachedInterpreter, Interpreter };
enum class FastMemoryMode : std::uint32_t { Safe, Fast, Unsafe };
enum class MemoryManagerMode : std::uint32_t {
    Software,
    HostMapped,
    HostMappedSafe,
    HostMappedUnsafe,
};
enum class PersistentCacheMode : std::uint32_t { Off, On, Rebuild };
enum class Aggressiveness : std::uint32_t { Off, Safe, Aggressive };
enum class CycleSkipMode : std::uint32_t { Off, Mild, Moderate, Aggressive };
enum class BlockSizeMode : std::uint32_t { Safe, Mega, Giga };
enum class FpAccuracyMode : std::uint32_t { Accurate, Approximate, Relaxed };
enum class AtomicMode : std::uint32_t { Strict, Compatible, Fast };
enum class SchedulerMode : std::uint32_t { Os, Emulator, Performance, Auto };
enum class CpuGpuSyncMode : std::uint32_t { Accurate, Adaptive, Relaxed };
enum class HleMode : std::uint32_t { Auto, PreferHle, PreferLle };
enum class FeatureMaskMode : std::uint32_t { Default, Compatibility, Custom };

struct ProfileV1 {
    std::uint32_t struct_size{sizeof(ProfileV1)};
    std::uint32_t version{1};
    Preset preset{Preset::Balanced};
    ExecutionMode execution_mode{ExecutionMode::Jit};
    FastMemoryMode fast_memory{FastMemoryMode::Safe};
    MemoryManagerMode memory_manager{MemoryManagerMode::HostMapped};
    std::uint32_t block_linking{1};
    PersistentCacheMode persistent_cache{PersistentCacheMode::On};
    Aggressiveness function_replacements{Aggressiveness::Safe};
    std::uint32_t guest_clock_percent{100};
    CycleSkipMode cycle_skip{CycleSkipMode::Off};
    Aggressiveness idle_loop_skip{Aggressiveness::Safe};
    Aggressiveness wait_loop_detection{Aggressiveness::Safe};
    Aggressiveness spin_loop_detection{Aggressiveness::Safe};


    std::uint32_t max_guest_workers{0};
    std::uint32_t fast_event_timing{1};
    BlockSizeMode jit_block_size{BlockSizeMode::Mega};
    FpAccuracyMode fp_accuracy{FpAccuracyMode::Accurate};
    AtomicMode atomics{AtomicMode::Compatible};
    std::uint32_t accurate_cache_line_stores{0};
    std::uint32_t accurate_dma_timing{0};
    std::uint32_t memory_exceptions{1};
    std::uint32_t icache_accuracy{1};
    std::uint32_t ignore_bad_accesses{0};
    FeatureMaskMode jit_feature_mask{FeatureMaskMode::Default};
    std::uint32_t jit_disable_mask{0};
    std::uint32_t cpu_gpu_dual_thread{2};
    SchedulerMode scheduler{SchedulerMode::Auto};
    std::uint32_t preferred_heavy_threads{0};
    std::uint32_t subsystem_multithreading{2};
    std::uint32_t separate_io_thread{2};
    std::uint32_t separate_audio_hle_thread{2};
    std::uint32_t guest_thread_delay_mode{0};
    std::uint32_t guest_thread_delay_us{0};
    std::uint32_t sync_on_idle_skip{1};
    CpuGpuSyncMode cpu_gpu_sync{CpuGpuSyncMode::Adaptive};
    std::uint32_t force_real_clock_sync{0};
    HleMode hle_mode{HleMode::Auto};
    std::uint32_t hle_fast_timers{1};
    std::uint32_t per_game_profile{2};
    std::uint32_t auto_game_db{1};


    std::uint32_t reserved0{};
    std::uint64_t identity{};
};

static_assert(std::is_trivially_copyable_v<ProfileV1>);
static_assert(std::is_standard_layout_v<ProfileV1>);
static_assert(sizeof(ProfileV1) == 176);
static_assert(offsetof(ProfileV1, reserved0) == 164);
static_assert(offsetof(ProfileV1, identity) == 168);

enum class Counter : std::size_t {
    IdleYields,
    WaitParks,
    SpinParks,
    CyclesSkipped,
    BadAccessesIgnored,
    HleFastCalls,
    HleAccurateCalls,
    DmaDelays,
    GuestThreadsDelayed,
    WorkerStartsThrottled,
    CpuGpuSyncs,
    Count,
};

inline ProfileV1& MutableProfile() {
    static ProfileV1 profile{};
    return profile;
}

inline std::mutex& ProfileMutex() {
    static std::mutex mutex;
    return mutex;
}

inline std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(Counter::Count)>&
Counters() {
    static std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(Counter::Count)> counters{};
    return counters;
}

inline void Count(const Counter counter, const std::uint64_t amount = 1) noexcept {
    Counters()[static_cast<std::size_t>(counter)].fetch_add(amount, std::memory_order_relaxed);
}

inline std::uint64_t CounterValue(const Counter counter) noexcept {
    return Counters()[static_cast<std::size_t>(counter)].load(std::memory_order_relaxed);
}

template <typename Enum>
inline Enum ClampEnum(const std::uint32_t value, const std::uint32_t maximum) {
    return static_cast<Enum>(std::min(value, maximum));
}

inline std::uint64_t HashProfile(const ProfileV1& source) noexcept {
    ProfileV1 copy = source;
    copy.reserved0 = 0;
    copy.identity = 0;
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(&copy);
    std::uint64_t hash = UINT64_C(1469598103934665603);
    for (std::size_t index = 0; index < sizeof(copy); ++index) {
        hash ^= bytes[index];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

inline ProfileV1 PresetProfile(const Preset preset) {
    ProfileV1 profile{};
    profile.preset = preset;
    switch (preset) {
    case Preset::Compatibility:
        profile.execution_mode = ExecutionMode::IrJit;
        profile.fast_memory = FastMemoryMode::Safe;
        profile.memory_manager = MemoryManagerMode::Software;
        profile.block_linking = 0;
        profile.function_replacements = Aggressiveness::Off;
        profile.idle_loop_skip = Aggressiveness::Safe;
        profile.wait_loop_detection = Aggressiveness::Safe;
        profile.spin_loop_detection = Aggressiveness::Safe;
        profile.fast_event_timing = 0;
        profile.jit_block_size = BlockSizeMode::Safe;
        profile.fp_accuracy = FpAccuracyMode::Accurate;
        profile.atomics = AtomicMode::Strict;
        profile.accurate_cache_line_stores = 1;
        profile.accurate_dma_timing = 1;
        profile.icache_accuracy = 1;
        profile.cpu_gpu_dual_thread = 0;
        profile.scheduler = SchedulerMode::Os;
        profile.jit_feature_mask = FeatureMaskMode::Compatibility;
        profile.subsystem_multithreading = 0;
        profile.separate_io_thread = 0;
        profile.separate_audio_hle_thread = 0;
        profile.cpu_gpu_sync = CpuGpuSyncMode::Accurate;
        profile.force_real_clock_sync = 1;
        profile.hle_fast_timers = 0;
        break;
    case Preset::Balanced:
        profile.fast_memory = FastMemoryMode::Fast;
        profile.hle_mode = HleMode::PreferHle;
        break;
    case Preset::Performance:
        profile.fast_memory = FastMemoryMode::Fast;
        profile.function_replacements = Aggressiveness::Aggressive;
        profile.idle_loop_skip = Aggressiveness::Aggressive;
        profile.wait_loop_detection = Aggressiveness::Aggressive;
        profile.spin_loop_detection = Aggressiveness::Aggressive;
        profile.guest_clock_percent = 85;
        profile.max_guest_workers = 4;
        profile.fast_event_timing = 2;
        profile.jit_block_size = BlockSizeMode::Giga;
        profile.fp_accuracy = FpAccuracyMode::Approximate;
        profile.atomics = AtomicMode::Fast;
        profile.memory_exceptions = 0;
        profile.icache_accuracy = 0;
        profile.scheduler = SchedulerMode::Performance;
        profile.preferred_heavy_threads = 4;
        profile.cpu_gpu_sync = CpuGpuSyncMode::Relaxed;
        profile.hle_mode = HleMode::PreferHle;
        profile.hle_fast_timers = 2;
        break;
    case Preset::Extreme:
        profile = PresetProfile(Preset::Performance);
        profile.preset = Preset::Extreme;
        profile.fast_memory = FastMemoryMode::Unsafe;
        profile.memory_manager = MemoryManagerMode::HostMappedUnsafe;
        profile.guest_clock_percent = 65;
        profile.cycle_skip = CycleSkipMode::Mild;
        profile.fp_accuracy = FpAccuracyMode::Relaxed;
        profile.sync_on_idle_skip = 0;
        profile.hle_fast_timers = 2;
        break;
    case Preset::Custom:
        profile.preset = Preset::Custom;
        break;
    }
    profile.identity = HashProfile(profile);
    return profile;
}

inline void Normalize(ProfileV1& profile) {
    profile.struct_size = sizeof(ProfileV1);
    profile.version = 1;
    profile.preset = ClampEnum<Preset>(static_cast<std::uint32_t>(profile.preset), 4);
    profile.execution_mode = ClampEnum<ExecutionMode>(static_cast<std::uint32_t>(profile.execution_mode), 3);
    profile.fast_memory = ClampEnum<FastMemoryMode>(static_cast<std::uint32_t>(profile.fast_memory), 2);
    profile.memory_manager = ClampEnum<MemoryManagerMode>(static_cast<std::uint32_t>(profile.memory_manager), 3);
    profile.persistent_cache = ClampEnum<PersistentCacheMode>(static_cast<std::uint32_t>(profile.persistent_cache), 2);
    profile.function_replacements = ClampEnum<Aggressiveness>(static_cast<std::uint32_t>(profile.function_replacements), 2);
    profile.cycle_skip = ClampEnum<CycleSkipMode>(static_cast<std::uint32_t>(profile.cycle_skip), 3);
    profile.idle_loop_skip = ClampEnum<Aggressiveness>(static_cast<std::uint32_t>(profile.idle_loop_skip), 2);
    profile.wait_loop_detection = ClampEnum<Aggressiveness>(static_cast<std::uint32_t>(profile.wait_loop_detection), 2);
    profile.spin_loop_detection = ClampEnum<Aggressiveness>(static_cast<std::uint32_t>(profile.spin_loop_detection), 2);
    profile.jit_block_size = ClampEnum<BlockSizeMode>(static_cast<std::uint32_t>(profile.jit_block_size), 2);
    profile.fp_accuracy = ClampEnum<FpAccuracyMode>(static_cast<std::uint32_t>(profile.fp_accuracy), 2);
    profile.atomics = ClampEnum<AtomicMode>(static_cast<std::uint32_t>(profile.atomics), 2);
    profile.jit_feature_mask = ClampEnum<FeatureMaskMode>(static_cast<std::uint32_t>(profile.jit_feature_mask), 2);
    profile.scheduler = ClampEnum<SchedulerMode>(static_cast<std::uint32_t>(profile.scheduler), 3);
    profile.cpu_gpu_sync = ClampEnum<CpuGpuSyncMode>(static_cast<std::uint32_t>(profile.cpu_gpu_sync), 2);
    profile.hle_mode = ClampEnum<HleMode>(static_cast<std::uint32_t>(profile.hle_mode), 2);
    profile.guest_clock_percent = std::clamp(profile.guest_clock_percent, 25U, 400U);
    profile.max_guest_workers = std::min(profile.max_guest_workers, 32U);
    profile.preferred_heavy_threads = std::min(profile.preferred_heavy_threads, 8U);
    profile.guest_thread_delay_us = std::min(profile.guest_thread_delay_us, 10000U);
    profile.jit_disable_mask &= (UINT32_C(1) << 18) - 1;
    auto bit = [](std::uint32_t& value) { value = value != 0 ? 1U : 0U; };
    auto tri = [](std::uint32_t& value) { value = std::min(value, 2U); };
    bit(profile.block_linking);
    tri(profile.fast_event_timing);
    bit(profile.accurate_cache_line_stores);
    bit(profile.accurate_dma_timing);
    bit(profile.memory_exceptions);
    bit(profile.icache_accuracy);
    bit(profile.ignore_bad_accesses);
    tri(profile.cpu_gpu_dual_thread);
    tri(profile.subsystem_multithreading);
    tri(profile.separate_io_thread);
    tri(profile.separate_audio_hle_thread);
    bit(profile.guest_thread_delay_mode);
    bit(profile.sync_on_idle_skip);
    bit(profile.force_real_clock_sync);
    tri(profile.hle_fast_timers);
    tri(profile.per_game_profile);
    bit(profile.auto_game_db);


    if (profile.fast_memory != FastMemoryMode::Unsafe ||
        profile.memory_manager != MemoryManagerMode::HostMappedUnsafe) {
        profile.ignore_bad_accesses = 0;
    }



    if (profile.ignore_bad_accesses != 0) {
        profile.execution_mode = ExecutionMode::IrJit;
    }



    if (profile.execution_mode == ExecutionMode::Jit &&
        (profile.fast_memory == FastMemoryMode::Safe ||
         profile.memory_manager == MemoryManagerMode::Software ||
         profile.memory_manager == MemoryManagerMode::HostMappedSafe)) {
        profile.execution_mode = ExecutionMode::IrJit;
    }
    if (profile.execution_mode == ExecutionMode::CachedInterpreter ||
        profile.execution_mode == ExecutionMode::Interpreter) {
        profile.block_linking = 0;
        profile.jit_block_size = BlockSizeMode::Safe;
    }
    if (profile.execution_mode == ExecutionMode::Interpreter) {
        profile.persistent_cache = PersistentCacheMode::Off;
    }
    if (profile.force_real_clock_sync != 0) {
        profile.guest_clock_percent = 100;
        profile.cycle_skip = CycleSkipMode::Off;
    }
    if (profile.jit_feature_mask != FeatureMaskMode::Custom) {
        profile.jit_disable_mask = 0;
    }
    profile.reserved0 = 0;
    profile.identity = HashProfile(profile);
}

inline std::uint32_t EffectiveJitDisableMask(const ProfileV1& profile) noexcept {
    if (profile.jit_feature_mask == FeatureMaskMode::Custom) {
        return profile.jit_disable_mask;
    }
    if (profile.jit_feature_mask == FeatureMaskMode::Compatibility) {
        return (UINT32_C(1) << 2) | (UINT32_C(1) << 5) |
               (UINT32_C(1) << 6) | (UINT32_C(1) << 7) |
               (UINT32_C(1) << 14) | (UINT32_C(1) << 17);
    }
    return 0;
}

inline void ResolveAutomaticRuntimePolicy(ProfileV1& profile) {
    const std::uint32_t logical = std::max(1U, std::thread::hardware_concurrency());
    if (profile.max_guest_workers == 0) {
        profile.max_guest_workers = std::clamp(logical > 4 ? logical - 2 : logical,
                                               2U, 6U);
    }
    if (profile.preferred_heavy_threads == 0) {
        profile.preferred_heavy_threads = std::clamp((logical + 1) / 2, 1U, 4U);
    }
    const auto resolve_auto = [logical](std::uint32_t& value,
                                        const std::uint32_t threshold) {
        if (value == 2) value = logical >= threshold ? 1U : 0U;
    };
    resolve_auto(profile.cpu_gpu_dual_thread, 6);
    resolve_auto(profile.subsystem_multithreading, 6);
    resolve_auto(profile.separate_io_thread, 6);
    resolve_auto(profile.separate_audio_hle_thread, 8);
    if (profile.guest_thread_delay_mode == 0) {
        const std::uint32_t oversubscription =
            profile.max_guest_workers > profile.preferred_heavy_threads
                ? profile.max_guest_workers - profile.preferred_heavy_threads
                : 0;
        profile.guest_thread_delay_us = std::min(oversubscription * 25U, 500U);
    }
    Normalize(profile);
}

inline std::uint64_t TranslationCacheIdentity(ProfileV1 profile) noexcept {
    if (profile.persistent_cache == PersistentCacheMode::Rebuild) {
        profile.persistent_cache = PersistentCacheMode::On;
    }

    profile.preset = Preset::Custom;
    profile.per_game_profile = 0;
    profile.auto_game_db = 0;
    profile.identity = 0;
    return HashProfile(profile);
}

inline bool ParseUnsigned(const std::string_view text, std::uint32_t& value) {
    std::uint32_t parsed{};
    const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
        return false;
    }
    value = parsed;
    return true;
}

template <auto Member>
inline void AssignProfileEnum(ProfileV1& profile, const std::uint32_t value) noexcept {
    using Value = std::remove_reference_t<decltype(profile.*Member)>;
    profile.*Member = static_cast<Value>(value);
}

inline bool LoadFromFile(const std::filesystem::path& path, ProfileV1& output,
                         std::string* error = nullptr) {
    std::ifstream input(path);
    if (!input) {
        if (error) *error = "cpu-profile-open-failed";
        return false;
    }
    std::unordered_map<std::string, std::uint32_t*> fields{
        {"block_linking", &output.block_linking},
        {"guest_clock_percent", &output.guest_clock_percent},
        {"max_guest_workers", &output.max_guest_workers},
        {"fast_event_timing", &output.fast_event_timing},
        {"accurate_cache_line_stores", &output.accurate_cache_line_stores},
        {"accurate_dma_timing", &output.accurate_dma_timing},
        {"memory_exceptions", &output.memory_exceptions},
        {"icache_accuracy", &output.icache_accuracy},
        {"ignore_bad_accesses", &output.ignore_bad_accesses},
        {"jit_disable_mask", &output.jit_disable_mask},
        {"cpu_gpu_dual_thread", &output.cpu_gpu_dual_thread},
        {"preferred_heavy_threads", &output.preferred_heavy_threads},
        {"subsystem_multithreading", &output.subsystem_multithreading},
        {"separate_io_thread", &output.separate_io_thread},
        {"separate_audio_hle_thread", &output.separate_audio_hle_thread},
        {"guest_thread_delay_mode", &output.guest_thread_delay_mode},
        {"guest_thread_delay_us", &output.guest_thread_delay_us},
        {"sync_on_idle_skip", &output.sync_on_idle_skip},
        {"force_real_clock_sync", &output.force_real_clock_sync},
        {"hle_fast_timers", &output.hle_fast_timers},
        {"per_game_profile", &output.per_game_profile},
        {"auto_game_db", &output.auto_game_db},
    };
    struct EnumField {
        std::string_view name;
        void (*assign)(ProfileV1&, std::uint32_t) noexcept;
    };
    static constexpr std::array enum_fields{
        EnumField{"preset", AssignProfileEnum<&ProfileV1::preset>},
        EnumField{"execution_mode", AssignProfileEnum<&ProfileV1::execution_mode>},
        EnumField{"fast_memory", AssignProfileEnum<&ProfileV1::fast_memory>},
        EnumField{"memory_manager", AssignProfileEnum<&ProfileV1::memory_manager>},
        EnumField{"persistent_cache", AssignProfileEnum<&ProfileV1::persistent_cache>},
        EnumField{"function_replacements", AssignProfileEnum<&ProfileV1::function_replacements>},
        EnumField{"cycle_skip", AssignProfileEnum<&ProfileV1::cycle_skip>},
        EnumField{"idle_loop_skip", AssignProfileEnum<&ProfileV1::idle_loop_skip>},
        EnumField{"wait_loop_detection", AssignProfileEnum<&ProfileV1::wait_loop_detection>},
        EnumField{"spin_loop_detection", AssignProfileEnum<&ProfileV1::spin_loop_detection>},
        EnumField{"jit_block_size", AssignProfileEnum<&ProfileV1::jit_block_size>},
        EnumField{"fp_accuracy", AssignProfileEnum<&ProfileV1::fp_accuracy>},
        EnumField{"atomics", AssignProfileEnum<&ProfileV1::atomics>},
        EnumField{"jit_feature_mask", AssignProfileEnum<&ProfileV1::jit_feature_mask>},
        EnumField{"scheduler", AssignProfileEnum<&ProfileV1::scheduler>},
        EnumField{"cpu_gpu_sync", AssignProfileEnum<&ProfileV1::cpu_gpu_sync>},
        EnumField{"hle_mode", AssignProfileEnum<&ProfileV1::hle_mode>},
    };
    auto assign_enum = [&](const std::string_view key, const std::uint32_t value) {
        const auto field = std::find_if(enum_fields.begin(), enum_fields.end(),
            [&](const EnumField& candidate) { return candidate.name == key; });
        if (field == enum_fields.end()) return false;
        field->assign(output, value);
        return true;
    };
    std::string line;
    std::uint32_t line_number{};
    while (std::getline(input, line)) {
        ++line_number;
        if (line.empty() || std::string_view{"#;["}.find(line.front()) != std::string_view::npos) continue;
        const auto separator = line.find('=');
        if (separator == std::string::npos) continue;
        const std::string key = line.substr(0, separator);
        const std::string_view value{line.data() + separator + 1, line.size() - separator - 1};
        std::uint32_t parsed{};
        const auto field = fields.find(key);
        const bool parsed_ok = ParseUnsigned(value, parsed);
        if (!parsed_ok ||
            (field == fields.end() && !assign_enum(key, parsed))) {
            if (error) *error = "cpu-profile-invalid-line-" + std::to_string(line_number);
            return false;
        }
        if (field != fields.end()) {
            *field->second = parsed;
        }
    }
    Normalize(output);
    return true;
}

inline bool ConfigureFromRuntimeRoot(const std::filesystem::path& root,
                                     std::string* error = nullptr) {
    ProfileV1 profile = PresetProfile(Preset::Balanced);
    if (!LoadFromFile(root / "runtime-config" / "android-cpu-tuning-v1.ini",
                      profile, error)) {
        return false;
    }
    ResolveAutomaticRuntimePolicy(profile);
    std::lock_guard lock(ProfileMutex());
    MutableProfile() = profile;
    return true;
}

inline ProfileV1 Get() noexcept {
    std::lock_guard lock(ProfileMutex());
    return MutableProfile();
}




inline const ProfileV1& Frozen() noexcept {
    return MutableProfile();
}

inline void SetForTesting(ProfileV1 profile) noexcept {
    Normalize(profile);
    std::lock_guard lock(ProfileMutex());
    MutableProfile() = profile;
}

inline std::uint32_t CycleSkipDivisor(const ProfileV1& profile) noexcept {
    switch (profile.cycle_skip) {
    case CycleSkipMode::Mild: return 16;
    case CycleSkipMode::Moderate: return 8;
    case CycleSkipMode::Aggressive: return 4;
    case CycleSkipMode::Off: return 0;
    }
    return 0;
}

}
