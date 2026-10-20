// SPDX-FileCopyrightText: Copyright 2026 LSX4
// SPDX-License-Identifier: GPL-2.0-or-later

#include "executor/cpu_tuning.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {

using namespace Executor::CpuTuning;

[[noreturn]] void Fail(const char* message) {
    std::cerr << "cpu-tuning-contract: " << message << '\n';
    std::exit(1);
}

void Require(const bool condition, const char* message) {
    if (!condition) Fail(message);
}

void CheckPresetsAndSafetyContracts() {
    const auto compatibility = PresetProfile(Preset::Compatibility);
    Require(compatibility.execution_mode == ExecutionMode::IrJit,
            "compatibility must use the checked LSX4 core");
    Require(compatibility.fast_memory == FastMemoryMode::Safe &&
                compatibility.memory_manager == MemoryManagerMode::Software,
            "compatibility memory contract weakened");
    Require(compatibility.cpu_gpu_sync == CpuGpuSyncMode::Accurate,
            "compatibility submit synchronization weakened");

    const auto performance = PresetProfile(Preset::Performance);
    Require(performance.execution_mode == ExecutionMode::Jit &&
                performance.scheduler == SchedulerMode::Performance,
            "performance preset no longer selects FEX/performance scheduling");
    Require(performance.ignore_bad_accesses == 0,
            "performance preset must not hide guest faults");

    const auto extreme = PresetProfile(Preset::Extreme);
    Require(extreme.fast_memory == FastMemoryMode::Unsafe &&
                extreme.memory_manager == MemoryManagerMode::HostMappedUnsafe,
            "extreme unsafe pair is incomplete");
    Require(extreme.ignore_bad_accesses == 0,
            "extreme must require a separate explicit bad-access opt-in");

    auto checked_fex = performance;
    checked_fex.fast_memory = FastMemoryMode::Safe;
    Normalize(checked_fex);
    Require(checked_fex.execution_mode == ExecutionMode::IrJit,
            "FEX cannot claim the checked-memory contract");

    auto bad_access = extreme;
    bad_access.ignore_bad_accesses = 1;
    Normalize(bad_access);
    Require(bad_access.execution_mode == ExecutionMode::IrJit,
            "precise bad-access replay must use LSX4 IR");

    auto invalid_bad_access = performance;
    invalid_bad_access.ignore_bad_accesses = 1;
    Normalize(invalid_bad_access);
    Require(invalid_bad_access.ignore_bad_accesses == 0,
            "bad-access replay escaped the unsafe pair");

    auto interpreter = performance;
    interpreter.execution_mode = ExecutionMode::Interpreter;
    Normalize(interpreter);
    Require(interpreter.block_linking == 0 &&
                interpreter.jit_block_size == BlockSizeMode::Safe &&
                interpreter.persistent_cache == PersistentCacheMode::Off,
            "fresh interpreter retained JIT-only state");

    auto real_clock = extreme;
    real_clock.force_real_clock_sync = 1;
    Normalize(real_clock);
    Require(real_clock.guest_clock_percent == 100 &&
                real_clock.cycle_skip == CycleSkipMode::Off,
            "real-clock mode retained synthetic guest timing");
}

void CheckResolutionAndCacheIdentity() {
    auto profile = PresetProfile(Preset::Balanced);
    ResolveAutomaticRuntimePolicy(profile);
    Require(profile.max_guest_workers >= 2 && profile.max_guest_workers <= 6,
            "automatic worker cap outside documented range");
    Require(profile.preferred_heavy_threads >= 1 &&
                profile.preferred_heavy_threads <= 4,
            "automatic preferred-core count outside documented range");
    Require(profile.cpu_gpu_dual_thread <= 1 &&
                profile.subsystem_multithreading <= 1 &&
                profile.separate_io_thread <= 1 &&
                profile.separate_audio_hle_thread <= 1,
            "automatic tri-state survived runtime resolution");

    auto cache_a = profile;
    cache_a.preset = Preset::Balanced;
    cache_a.per_game_profile = 2;
    cache_a.auto_game_db = 1;
    cache_a.persistent_cache = PersistentCacheMode::Rebuild;
    auto cache_b = cache_a;
    cache_b.preset = Preset::Custom;
    cache_b.per_game_profile = 0;
    cache_b.auto_game_db = 0;
    cache_b.persistent_cache = PersistentCacheMode::On;
    Require(TranslationCacheIdentity(cache_a) == TranslationCacheIdentity(cache_b),
            "source-selection metadata changed translation-cache identity");

    const auto canonical_identity = profile.identity;
    profile.reserved0 = 0x6f;
    Normalize(profile);
    Require(profile.reserved0 == 0 && profile.identity == canonical_identity,
            "CPU profile identity depends on the pre-identity alignment slot");
}

void CheckParser() {
    const auto directory = std::filesystem::temp_directory_path() /
                           "lsx4-cpu-tuning-profile-contract";
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) Fail("could not create temporary directory");
    const auto valid = directory / "valid.ini";
    {
        std::ofstream stream(valid, std::ios::trunc);
        stream << "preset=4\n"
                  "execution_mode=0\n"
                  "fast_memory=0\n"
                  "memory_manager=1\n"
                  "block_linking=1\n"
                  "persistent_cache=2\n"
                  "function_replacements=2\n"
                  "guest_clock_percent=900\n"
                  "cycle_skip=3\n"
                  "idle_loop_skip=2\n"
                  "wait_loop_detection=2\n"
                  "spin_loop_detection=2\n"
                  "max_guest_workers=99\n"
                  "fast_event_timing=2\n"
                  "jit_block_size=2\n"
                  "fp_accuracy=2\n"
                  "atomics=2\n"
                  "accurate_cache_line_stores=1\n"
                  "accurate_dma_timing=1\n"
                  "memory_exceptions=1\n"
                  "icache_accuracy=1\n"
                  "ignore_bad_accesses=1\n"
                  "jit_feature_mask=1\n"
                  "jit_disable_mask=262143\n"
                  "cpu_gpu_dual_thread=2\n"
                  "scheduler=3\n"
                  "preferred_heavy_threads=99\n"
                  "subsystem_multithreading=2\n"
                  "separate_io_thread=2\n"
                  "separate_audio_hle_thread=2\n"
                  "guest_thread_delay_mode=1\n"
                  "guest_thread_delay_us=99999\n"
                  "sync_on_idle_skip=1\n"
                  "cpu_gpu_sync=2\n"
                  "force_real_clock_sync=1\n"
                  "hle_mode=2\n"
                  "hle_fast_timers=2\n"
                  "per_game_profile=2\n"
                  "auto_game_db=1\n";
    }
    ProfileV1 parsed = PresetProfile(Preset::Balanced);
    std::string message;
    Require(LoadFromFile(valid, parsed, &message), "complete profile failed to parse");
    Require(parsed.execution_mode == ExecutionMode::IrJit,
            "checked memory did not normalize FEX to LSX4 IR");
    Require(parsed.guest_clock_percent == 100 &&
                parsed.cycle_skip == CycleSkipMode::Off,
            "parser normalization did not enforce real-clock mode");
    Require(parsed.max_guest_workers == 32 &&
                parsed.preferred_heavy_threads == 8 &&
                parsed.guest_thread_delay_us == 10000,
            "numeric profile bounds were not clamped");
    Require(parsed.jit_disable_mask == 0,
            "non-custom feature-mask mode retained a custom disable mask");

    const auto invalid = directory / "invalid.ini";
    {
        std::ofstream stream(invalid, std::ios::trunc);
        stream << "execution_mode=not-a-number\n";
    }
    parsed = PresetProfile(Preset::Balanced);
    Require(!LoadFromFile(invalid, parsed, &message),
            "malformed numeric profile value was accepted");
    std::filesystem::remove_all(directory, error);
}

} // namespace

int main() {
    static_assert(sizeof(ProfileV1) == 176);
    static_assert(offsetof(ProfileV1, reserved0) == 164);
    static_assert(offsetof(ProfileV1, identity) == 168);
    CheckPresetsAndSafetyContracts();
    CheckResolutionAndCacheIdentity();
    CheckParser();
    std::cout << "cpu-tuning-contract: ok\n";
    return 0;
}
