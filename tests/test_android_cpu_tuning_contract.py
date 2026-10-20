import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
FEX_ROOT = ROOT.parent / "fex-emu-cpu-hacks-8gen5-wt"
ADAPTER_ROOT = ROOT.parent / "fex-adapter-cpu-hacks-8gen5-wt"


def source(root: Path, relative: str) -> str:
    return (root / relative).read_text(encoding="utf-8")


FIELDS = (
    "execution_mode", "fast_memory", "memory_manager", "block_linking",
    "persistent_cache", "function_replacements", "guest_clock_percent",
    "cycle_skip", "idle_loop_skip", "wait_loop_detection",
    "spin_loop_detection", "max_guest_workers", "fast_event_timing",
    "jit_block_size", "fp_accuracy", "atomics",
    "accurate_cache_line_stores", "accurate_dma_timing",
    "memory_exceptions", "icache_accuracy", "ignore_bad_accesses",
    "jit_feature_mask", "jit_disable_mask", "cpu_gpu_dual_thread",
    "scheduler", "preferred_heavy_threads", "subsystem_multithreading",
    "separate_io_thread", "separate_audio_hle_thread",
    "guest_thread_delay_mode", "guest_thread_delay_us", "sync_on_idle_skip",
    "cpu_gpu_sync", "force_real_clock_sync", "hle_mode", "hle_fast_timers",
    "per_game_profile", "auto_game_db",
)


class AndroidCpuTuningContractTests(unittest.TestCase):
    def test_every_ui_field_is_serialized_and_parsed(self):
        java = source(
            ROOT,
            "android-app/app/src/main/java/app/lsx4/android/CpuTuningSettings.java",
        )
        profile = source(ROOT, "src/executor/cpu_tuning.h")
        ui_fields = re.findall(r'new Field\("([a-z0-9_]+)"', java)
        self.assertEqual(list(FIELDS), ui_fields)
        for field in FIELDS:
            self.assertIn(f'output.append(field.key)', java)
            self.assertRegex(profile, rf'"{field}"')
        self.assertIn('.append("preset=")', java)
        self.assertIn('if (autoGameDb != 0)', java)
        resolution = java.split("static Map<String, Integer> resolvedValues", 1)[1].split(
            "private static boolean shouldUseGameScope", 1
        )[0]
        self.assertLess(
            resolution.index("applyGameDb(values"),
            resolution.index("overlayExplicitValues(preferences, GLOBAL_PREFIX"),
        )
        self.assertLess(
            resolution.index("overlayExplicitValues(preferences, GLOBAL_PREFIX"),
            resolution.index("overlayExplicitValues(preferences, scope"),
        )
        self.assertLess(
            resolution.index("overlayExplicitValues(preferences, scope"),
            resolution.index("enforceContracts(values)"),
        )

    def test_bloodborne_keeps_its_complete_havok_worker_cohort(self):
        java = source(
            ROOT,
            "android-app/app/src/main/java/app/lsx4/android/CpuTuningSettings.java",
        )
        game_db = java.split("private static void applyGameDb", 1)[1].split(
            "private static void enforceContracts", 1
        )[0]
        self.assertIn('final boolean bloodborne = "CUSA03173".equals(titleId)', game_db)
        self.assertIn(
            'values.put("max_guest_workers", bloodborne ? 6 : 4)', game_db
        )
        self.assertIn('"CUSA10349".equals(titleId)', game_db)
        self.assertIn('"CUSA05667".equals(titleId)', game_db)
        self.assertIn('"CUSA30277".equals(titleId)', game_db)
        self.assertIn('"CUSA10606".equals(titleId)', game_db)
        self.assertIn('"CUSA11628".equals(titleId)', game_db)
        self.assertIn('"CUSA53312".equals(titleId)', game_db)
        self.assertIn('values.put("max_guest_workers", 32)', game_db)
        short_hike = game_db.split(
            'if ("CUSA30277".equals(titleId)) {', 1
        )[1].split("}", 1)[0]
        self.assertIn('values.put("jit_block_size", 0)', short_hike)
        self.assertIn('values.put("block_linking", 1)', short_hike)
        little_kitty = game_db.split(
            'if ("CUSA53312".equals(titleId)) {', 1
        )[1].split("}", 1)[0]
        self.assertIn('values.put("jit_block_size", 0)', little_kitty)
        self.assertIn('values.put("block_linking", 1)', little_kitty)
        gonner = game_db.split(
            'if ("CUSA11628".equals(titleId)) {', 1
        )[1].split("}", 1)[0]
        self.assertIn('values.putAll(presetValues(PRESET_BALANCED))', gonner)
        self.assertIn('values.put("jit_block_size", 2)', gonner)
        self.assertIn('values.put("persistent_cache", 0)', gonner)
        self.assertIn('values.put("icache_accuracy", 0)', gonner)
        self.assertIn('values.put("hle_mode", 0)', gonner)
        self.assertIn('values.put("max_guest_workers", 32)', gonner)
        coffee_talk = game_db.split(
            'if ("CUSA18112".equals(titleId)) {', 1
        )[1].split("}", 1)[0]
        self.assertIn('values.putAll(presetValues(PRESET_BALANCED))', coffee_talk)
        self.assertIn('values.put("jit_block_size", 0)', coffee_talk)
        self.assertIn('values.put("persistent_cache", 0)', coffee_talk)
        self.assertIn('values.put("icache_accuracy", 0)', coffee_talk)
        self.assertIn('values.put("max_guest_workers", 32)', coffee_talk)
        moonscars = game_db.split(
            'if ("CUSA35013".equals(titleId)) {', 1
        )[1].split("}", 1)[0]
        self.assertIn('values.putAll(presetValues(PRESET_COMPATIBILITY))', moonscars)
        self.assertIn('values.put("max_guest_workers", 32)', moonscars)
        thomas = game_db.split(
            'if ("CUSA00909".equals(titleId)) {', 1
        )[1].split("}", 1)[0]
        self.assertIn('values.putAll(presetValues(PRESET_COMPATIBILITY))', thomas)
        self.assertIn('values.put("fast_memory", 2)', thomas)
        self.assertIn('values.put("memory_manager", 3)', thomas)
        self.assertIn('values.put("ignore_bad_accesses", 1)', thomas)
        self.assertIn('values.put("max_guest_workers", 32)', thomas)
        islets = game_db.split(
            'if ("CUSA52139".equals(titleId)) {', 1
        )[1].split("}", 1)[0]
        self.assertIn('values.putAll(presetValues(PRESET_BALANCED))', islets)
        self.assertIn('values.put("execution_mode", 1)', islets)
        hotline = game_db.split(
            'if ("CUSA00486".equals(titleId)) {', 1
        )[1].split("}", 1)[0]
        self.assertIn('values.putAll(presetValues(PRESET_COMPATIBILITY))', hotline)
        self.assertIn('values.put("fast_memory", 2)', hotline)
        self.assertIn('values.put("memory_manager", 3)', hotline)
        self.assertIn('values.put("ignore_bad_accesses", 1)', hotline)
        bad_north = game_db.split(
            'if ("CUSA10606".equals(titleId)) {', 1
        )[1].split("}", 1)[0]
        self.assertIn('values.putAll(presetValues(PRESET_COMPATIBILITY))', bad_north)
        self.assertIn('values.put("max_guest_workers", 32)', bad_north)
        self.assertIn('values.put("scheduler", bloodborne ? 0 : 2)', game_db)
        self.assertIn('values.put("block_linking", 1)', game_db)
        self.assertIn('values.put("jit_block_size", bloodborne ? 0 : 1)', game_db)
        self.assertIn('values.put("persistent_cache", bloodborne ? 0 : 1)', game_db)
        self.assertIn('values.put("icache_accuracy", bloodborne ? 0 : 1)', game_db)
        runtime = source(ROOT, "src/executor/native_runtime_api.cpp")
        self.assertIn(
            "cpu_profile.jit_block_size == Executor::CpuTuning::BlockSizeMode::Safe",
            runtime,
        )
        self.assertIn("LSX4_FEX_CONFIG_MAXINST_256_AB", runtime)
        adapter = source(ADAPTER_ROOT, "src/fex_context_bridge.cpp")
        self.assertIn("tuning.jit_block_size == 0 ? 256", adapter)
        self.assertIn("const bool maxinst_256 = max_inst == 256", adapter)
        for key in ("fast_event_timing", "atomics"):
            self.assertIn(f'values.put("{key}", 1)', game_db)
        self.assertIn('values.put("cpu_gpu_sync", 1)', game_db)
        self.assertIn('values.put("accurate_cache_line_stores", 0)', game_db)

    def test_profile_is_frozen_before_backend_activation(self):
        activity = source(
            ROOT, "android-app/app/src/main/java/app/lsx4/android/MainActivity.java"
        )
        load = activity.split("private void loadRuntimeFromFile", 1)[1].split(
            "private int applyNativeRuntimePaused", 1
        )[0]
        self.assertLess(
            load.index("CpuTuningSettings.materialize"),
            load.index("RuntimeBridge.load"),
        )
        native = source(ROOT, "src/executor/native_runtime_api.cpp")
        select = native.split("int SetGuestCpuBackend", 1)[1].split(
            'extern "C" int executor_lsx4_runtime_set_guest_cpu_backend', 1
        )[0]
        self.assertIn("ConfigureFromRuntimeRoot", select)
        self.assertIn("ResolveAutomaticRuntimePolicy(fallback_profile)", select)
        self.assertIn("Common::ConfigureCpuTuningPolicy", select)
        self.assertIn("preflight.cpu_tuning = {", select)

    def test_default_guest_clock_scaling_is_an_exact_pre_division_identity(self):
        policy = source(ROOT, "funnel-arm/src/common/cpu_tuning_policy.h")
        scale = policy.split("constexpr std::uint64_t ScaleGuestDuration", 1)[1].split(
            "static_assert(ScaleGuestDuration", 1
        )[0]
        identity = scale.index(
            "if (policy.guest_clock_percent == 100 && divisor == 0) return value;"
        )
        quotient = scale.index("const auto quotient = value / denominator;")
        remainder = scale.index("const auto remainder = value % denominator;")
        self.assertLess(identity, quotient)
        self.assertLess(identity, remainder)
        self.assertIn("const auto divisor = CpuCycleSkipDivisor(policy);", scale)
        self.assertEqual(scale.count("CpuCycleSkipDivisor(policy)"), 1)
        self.assertIn(
            "ScaleGuestDuration(~std::uint64_t{}, CpuTuningPolicy{})",
            policy,
        )

    def test_hle_hot_paths_read_the_prelaunch_frozen_profile_without_a_mutex(self):
        profile = source(ROOT, "src/executor/cpu_tuning.h")
        frozen = profile.split("inline const ProfileV1& Frozen() noexcept {", 1)[1].split("}", 1)[0]
        self.assertIn("return MutableProfile();", frozen)
        self.assertNotIn("lock_guard", frozen)
        self.assertNotIn("unique_lock", frozen)
        native = source(ROOT, "src/executor/native_runtime_api.cpp")
        hot_paths = (
            native.split("ExecutorMappedHleBridgeCallbackCommon(", 1)[1].split(
                "static std::uint64_t ExecutorHleBridgeCallbackCatching(", 1
            )[0],
            native.split("extern \"C\" int32_t ExecutorFexDispatchHle(", 1)[1].split(
                "void ExecutorFexThreadEvent", 1
            )[0],
            native.split("int32_t ExecutorFexTryDispatchLeafHle(", 1)[1].split(
                "const Lsx4FexHostCallbacksV3& ExecutorFexHostCallbacks", 1
            )[0],
        )
        for hot_path in hot_paths:
            self.assertIn("CpuTuning::Frozen().function_replacements", hot_path)
            self.assertNotIn("CpuTuning::Get()", hot_path)

    def test_memory_and_backend_contracts_are_enforced_twice(self):
        java = source(
            ROOT,
            "android-app/app/src/main/java/app/lsx4/android/CpuTuningSettings.java",
        )
        profile = source(ROOT, "src/executor/cpu_tuning.h")
        adapter = source(ADAPTER_ROOT, "src/fex_context_bridge.cpp")
        for implementation in (java, profile):
            self.assertIn("ignore_bad_accesses", implementation)
            self.assertIn("fast_memory", implementation)
            self.assertIn("memory_manager", implementation)
            self.assertIn("execution_mode", implementation)
        validator = adapter.split("bool IsCpuTuningValid", 1)[1].split(
            "return true;", 1
        )[0]
        self.assertIn("profile->execution_mode != 0", validator)
        self.assertIn("profile->fast_memory == 0", validator)
        self.assertIn("profile->memory_manager == 0", validator)
        self.assertIn("profile->memory_manager == 2", validator)
        self.assertIn("profile->ignore_bad_accesses != 0", validator)

    def test_worker_cap_is_an_admission_gate_not_create_failure(self):
        native = source(ROOT, "src/executor/native_runtime_api.cpp")
        should_fail = native.split(
            "executor_lsx4_android_should_fail_guest_thread_create", 1
        )[1].split("static bool RepairUnityPreloadControlBlock", 1)[0]
        self.assertNotIn("max_guest_workers", should_fail)
        admission = native.split(
            "executor_lsx4_android_wait_guest_worker_slot", 1
        )[1].split("executor_lsx4_android_guest_thread_delay_us", 1)[0]
        self.assertIn("condition.wait", admission)
        self.assertIn("active.size() < limit", admission)
        self.assertIn("active.insert(thread)", admission)
        self.assertIn("executor_lsx4_android_note_guest_thread_exit", native)
        pthread = source(
            ROOT, "funnel-arm/src/core/libraries/kernel/threads/pthread.cpp"
        )
        self.assertIn("executor_lsx4_android_wait_guest_worker_slot", pthread)
        self.assertIn("executor_lsx4_android_note_guest_thread_exit", pthread)

    def test_scheduler_modes_change_real_affinity_policy(self):
        thread = source(ROOT, "funnel-arm/src/common/thread.cpp")
        affinity = thread.split("bool ApplyCurrentThreadAffinity", 1)[1].split(
            "#ifdef __APPLE__", 1
        )[0]
        self.assertIn("if (scheduler_mode == 0)", affinity)
        self.assertIn("return true;", affinity)
        self.assertIn("requested_scheduler_mode == 3", affinity)
        self.assertIn("topology.usable", affinity)
        self.assertIn("scheduler_mode == 2", affinity)
        self.assertIn("performance_policy_cpus", affinity)
        self.assertIn("sched_setaffinity", affinity)

    def test_cpu_gpu_sync_and_dma_policy_reach_submission(self):
        header = source(ROOT, "funnel-arm/src/video_core/amdgpu/liverpool.h")
        implementation = source(ROOT, "funnel-arm/src/video_core/amdgpu/liverpool.cpp")
        combined = header + implementation
        self.assertIn("cpu_gpu_sync == 0", combined)
        self.assertIn("cpu_gpu_sync == 1", combined)
        self.assertIn("AdaptiveCpuGpuQueueHighWater = 64", combined)
        self.assertIn("AdaptiveCpuGpuQueueLowWater = 32", combined)
        self.assertIn(
            "published_command_count > AdaptiveCpuGpuQueueHighWater", combined
        )
        self.assertIn("num_submits.load(std::memory_order_acquire) == 0", implementation)
        self.assertIn("accurate_dma_timing", implementation)
        self.assertIn("CpuTuningEvent::DmaDelays", implementation)

    def test_optional_subsystem_threads_do_real_inline_or_worker_dispatch(self):
        aio = source(ROOT, "funnel-arm/src/core/libraries/kernel/aio.cpp")
        self.assertIn("class AioWorker", aio)
        self.assertIn("policy.separate_io_thread != 0", aio)
        self.assertIn("GetAioWorker().Submit", aio)
        self.assertIn("std::vector<OrbisKernelAioRWRequest> requests", aio)
        self.assertIn("usec == nullptr || *usec == 0", aio)
        audio = source(ROOT, "funnel-arm/src/core/libraries/audio/audioout.cpp")
        ajm = source(ROOT, "funnel-arm/src/core/libraries/ajm/ajm_context.cpp")
        self.assertIn("separate_audio_hle_thread", audio)
        self.assertIn("separate_audio_hle_thread", ajm)

    def test_hle_selection_changes_resolver_order(self):
        linker = source(ROOT, "funnel-arm/src/core/linker.cpp")
        prefer_hle = linker.index("GetCpuTuningPolicy().hle_mode == 1")
        guest_export = linker.index("const auto* p = FindExportedModule", prefer_hle)
        prefer_lle = linker.index("GetCpuTuningPolicy().hle_mode == 2", guest_export)
        self.assertLess(prefer_hle, guest_export)
        self.assertGreater(prefer_lle, guest_export)
        self.assertIn("m_hle_symbols.FindSymbol", linker[prefer_hle:guest_export])

    def test_fex_receives_fp_atomic_block_and_feature_controls(self):
        context = source(FEX_ROOT, "FEXCore/Source/Interface/Context/Context.h")
        pass_manager = source(FEX_ROOT, "FEXCore/Source/Interface/IR/PassManager.cpp")
        x87 = source(
            FEX_ROOT,
            "FEXCore/Source/Interface/IR/Passes/x87StackOptimizationPass.cpp",
        )
        bridge = source(ADAPTER_ROOT, "src/fex_context_bridge.cpp")
        self.assertIn("FPAccuracyMode", context)
        self.assertIn("FPAccuracyMode", context)
        self.assertIn("Config.FPAccuracyMode", pass_manager)
        self.assertIn("RelaxedPrecisionMode", x87)
        self.assertIn("tuning.block_linking != 0", bridge)
        self.assertIn("const bool strict_atomics = tuning.atomics == 0", bridge)
        self.assertIn("const bool fast_atomics = tuning.atomics == 2", bridge)
        self.assertIn("EffectiveJitDisableMask", source(ROOT, "src/executor/cpu_tuning.h"))

    def test_abi_and_hud_tail_are_large_enough_for_full_profile_proof(self):
        abi = source(ROOT, "src/executor/fex_backend_abi_v3.h")
        cpu_tuning = source(ROOT, "src/executor/cpu_tuning.h")
        runtime = source(ROOT, "src/executor/native_runtime_api.cpp")
        jni = source(ROOT, "android-app/app/src/main/cpp/runtime_bridge.cpp")
        proof = source(
            ROOT,
            "android-app/app/src/main/java/app/lsx4/android/ProofMetricsReceiver.java",
        )
        self.assertIn("LSX4_FEX_ABI_ASSERT_POD(Lsx4FexCpuTuningV1, 176)", abi)
        self.assertIn("LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexCpuTuningV1, reserved0, 164)", abi)
        self.assertIn("LSX4_FEX_ABI_ASSERT_OFFSET(Lsx4FexCpuTuningV1, identity, 168)", abi)
        self.assertIn("copy.reserved0 = 0;", cpu_tuning)
        self.assertIn("kValueCount = 166", runtime)
        self.assertIn("kValueCapacity = 166", jni)
        self.assertIn("values[165]", runtime)
        self.assertIn('"workerStartsThrottled"', proof)
        self.assertIn('"cpuGpuSyncs"', proof)
        self.assertIn('hudValues[155 + index]', proof)
        self.assertIn('.put("cpuTuningEventsEnd", 165)', proof)

    def test_cpu_settings_show_selected_and_effective_persisted_values(self):
        settings = source(
            ROOT,
            "android-app/app/src/main/java/app/lsx4/android/SettingsActivity.java",
        )
        tuning = source(
            ROOT,
            "android-app/app/src/main/java/app/lsx4/android/CpuTuningSettings.java",
        )
        controls = settings.split("private void renderCpuTuningControls", 1)[1].split(
            "private void buildManagedOptimizations", 1
        )[0]
        self.assertIn("CpuTuningSettings.selectedValues(preferences, titleId)", controls)
        self.assertIn("CpuTuningSettings.effectiveRuntimeValues(preferences, titleId)", controls)
        self.assertIn('selectedValues.get("preset")', controls)
        self.assertIn("Effective next launch:", controls)
        self.assertIn("enableCpuGameOverrideIfNeeded(titleId)", controls)
        self.assertIn("onStopTrackingTouch", controls)
        self.assertIn("renderCpuTuningControls(container, scope)", controls)
        self.assertNotIn(
            "CpuTuningSettings.read(\n                        prefs(SettingsActivity.this), scope, field)",
            controls,
        )

        scope_selection = tuning.split("private static boolean shouldUseGameScope", 1)[1].split(
            "private static void applyGameDb", 1
        )[0]
        self.assertIn("preferences.contains(enabledKey)", scope_selection)
        self.assertIn("preferences.getBoolean(enabledKey, false)", scope_selection)
        self.assertLess(
            scope_selection.index("preferences.contains(enabledKey)"),
            scope_selection.index("for (Field field : FIELDS)"),
        )

        auto = tuning.split("static void resolveAutomaticRuntimePolicy", 1)[1].split(
            "private static void overlayExplicitValues", 1
        )[0]
        self.assertIn("logical > 4 ? logical - 2 : logical", auto)
        self.assertIn("(logical + 1) / 2", auto)
        self.assertIn('resolveAutoThreadToggle(values, "cpu_gpu_dual_thread", logical, 6)', auto)
        self.assertIn('resolveAutoThreadToggle(values, "separate_audio_hle_thread", logical, 8)', auto)
        self.assertIn("Math.min(oversubscription * 25, 500)", auto)
        self.assertIn("enforceContracts(values)", auto)

        discovery = tuning.split("static List<String> installedTitleIds", 1)[1].split(
            "static int preset", 1
        )[0]
        self.assertIn("lsx4-home/runtime-fs/user/app", discovery)
        self.assertIn("lsx4-home/games", discovery)
        self.assertIn("SharedGameStorage.gamesDirectory()", discovery)
        self.assertIn("!result.contains(id)", discovery)

    def test_build_script_routes_to_the_local_cpu_hack_worktrees(self):
        build = source(ROOT, "tools/fex/build_android_fex_runtime.ps1")
        gradle = source(ROOT, "android-app/app/build.gradle")
        self.assertIn("fex-emu-cpu-hacks-8gen5-wt", build)
        self.assertIn("fex-adapter-cpu-hacks-8gen5-wt", build)
        self.assertIn("[string]$FexSource", build)
        self.assertIn("[string]$AdapterSource", build)
        self.assertIn("lsx4FexRuntimeManifest", gradle)


if __name__ == "__main__":
    unittest.main()
