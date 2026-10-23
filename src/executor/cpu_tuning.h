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

// Values are deliberately fixed-width: this profile is copied into the FEX
// adapter ABI before the process-global translator is activated.
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
    // Zero is Auto in the persisted/UI profile. It is resolved once, before
    // guest threads are created, and never changes while a game is running.
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
    // Occupies the natural alignment slot before identity. Keeping it explicit
    // prevents profile hashes from ever depending on indeterminate padding.
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
    // Unsafe memory is the only mode allowed to suppress a fault. Keeping the
    // two switches coupled prevents a stale preference from weakening Safe.
    if (profile.fast_memory != FastMemoryMode::Unsafe ||
        profile.memory_manager != MemoryManagerMode::HostMappedUnsafe) {
        profile.ignore_bad_accesses = 0;
    }
    // Exact fault replay is implemented by the LSX4 IR decoder/interpreter.
    // FEX cannot safely skip an arbitrary AArch64 fault without reconstructing
    // all guest-visible effects of the x86 instruction.
    if (profile.ignore_bad_accesses != 0) {
        profile.execution_mode = ExecutionMode::IrJit;
    }
    // The embedded FEX backend is a host-mapped translator. Checked/software
    // address translation is implemented by the LSX4 IR core, so requesting
    // either safe memory contract must select the engine that can enforce it.
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

inline void ResolveAutomaticRuntimePolicy(
    ProfileV1& profile,
    const std::uint32_t logical_processors = std::thread::hardware_concurrency()) {
    const std::uint32_t logical = std::max(1U, logical_processors);
    if (profile.max_guest_workers == 0) {
#if defined(LSX4_APPLE_RUNTIME)
        // Admission is a lifetime progress budget, not a CPU affinity count.
        // Four persistent Unity workers can all sleep while the loader workers
        // wait outside this gate. Preserve the six-slot Android auto budget on
        // six-core iPhones; placement below still uses the actual host cores.
        // Explicit GameDB and Custom limits bypass this automatic branch.
        profile.max_guest_workers = 6;
#else
        profile.max_guest_workers = std::clamp(logical > 4 ? logical - 2 : logical,
                                               2U, 6U);
#endif
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
    // These fields choose where values come from, not generated code.
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
    auto assign_enum = [&](const std::string& key, const std::uint32_t value) {
        if (key == "preset") output.preset = static_cast<Preset>(value);
        else if (key == "execution_mode") output.execution_mode = static_cast<ExecutionMode>(value);
        else if (key == "fast_memory") output.fast_memory = static_cast<FastMemoryMode>(value);
        else if (key == "memory_manager") output.memory_manager = static_cast<MemoryManagerMode>(value);
        else if (key == "persistent_cache") output.persistent_cache = static_cast<PersistentCacheMode>(value);
        else if (key == "function_replacements") output.function_replacements = static_cast<Aggressiveness>(value);
        else if (key == "cycle_skip") output.cycle_skip = static_cast<CycleSkipMode>(value);
        else if (key == "idle_loop_skip") output.idle_loop_skip = static_cast<Aggressiveness>(value);
        else if (key == "wait_loop_detection") output.wait_loop_detection = static_cast<Aggressiveness>(value);
        else if (key == "spin_loop_detection") output.spin_loop_detection = static_cast<Aggressiveness>(value);
        else if (key == "jit_block_size") output.jit_block_size = static_cast<BlockSizeMode>(value);
        else if (key == "fp_accuracy") output.fp_accuracy = static_cast<FpAccuracyMode>(value);
        else if (key == "atomics") output.atomics = static_cast<AtomicMode>(value);
        else if (key == "jit_feature_mask") output.jit_feature_mask = static_cast<FeatureMaskMode>(value);
        else if (key == "scheduler") output.scheduler = static_cast<SchedulerMode>(value);
        else if (key == "cpu_gpu_sync") output.cpu_gpu_sync = static_cast<CpuGpuSyncMode>(value);
        else if (key == "hle_mode") output.hle_mode = static_cast<HleMode>(value);
        else return false;
        return true;
    };
    std::string line;
    std::uint32_t line_number{};
    while (std::getline(input, line)) {
        ++line_number;
        if (line.empty() || line[0] == '#' || line[0] == ';' || line[0] == '[') continue;
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

// The Android launcher publishes the process profile before any guest thread or
// translator worker is started. Hot instruction helpers may use this immutable
// view; configuration and SetForTesting must never race guest execution.
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

} // namespace Executor::CpuTuning
