from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
THREAD_CPP = (ROOT / "funnel-arm/src/common/thread.cpp").read_text(
    encoding="utf-8"
)
THREAD_H = (ROOT / "funnel-arm/src/common/thread.h").read_text(
    encoding="utf-8"
)
NATIVE_RUNTIME = (ROOT / "src/executor/native_runtime_api.cpp").read_text(
    encoding="utf-8"
)
HOST_ISA_PROFILE = (
    ROOT / "src/executor/dynamic_translation/host_isa_profile.cpp"
).read_text(encoding="utf-8")
NATIVE_THREAD = (ROOT / "funnel-arm/src/core/thread.cpp").read_text(
    encoding="utf-8"
)
PAGE_MANAGER = (ROOT / "funnel-arm/src/video_core/page_manager.cpp").read_text(
    encoding="utf-8"
)


def function_body(source: str, signature: str) -> str:
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 0
    for index in range(brace, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[brace : index + 1]
    raise AssertionError(f"unterminated function: {signature}")


class Sm8650ThreadAffinityContractTest(unittest.TestCase):
    def test_primary_guest_policy_is_sm8650_and_x4_only(self):
        self.assertIn("PrimaryGuest = 3", THREAD_H)
        self.assertIn("topology.cortex_x4 && IsSm8650()", THREAD_CPP)
        self.assertIn('normalized == "sm8650"', THREAD_CPP)
        self.assertIn("((midr >> 24) & 0xffu) == 0x41u", THREAD_CPP)
        self.assertIn("((midr >> 4) & 0xfffu) == 0xd82u", THREAD_CPP)

        apply_body = function_body(
            THREAD_CPP, "bool ApplyCurrentThreadAffinity("
        )
        self.assertIn("case ThreadAffinityClass::PrimaryGuest", apply_body)
        self.assertIn(
            "topology.snapdragon_8_gen_3 ? topology.performance_cpus",
            apply_body,
        )
        self.assertIn(": topology.latency_cpus", apply_body)
        self.assertIn("case ThreadAffinityClass::GuestCompute", apply_body)
        self.assertIn("return topology.compute_cpus", apply_body)
        self.assertIn("case ThreadAffinityClass::GraphicsDriver", apply_body)
        self.assertIn("return topology.graphics_cpus", apply_body)

    def test_fex_memory_pipeline_requires_sm8650_x4_and_no_hardware_tso(self):
        host_query = function_body(THREAD_CPP, "bool IsSnapdragon8Gen3Host()")
        self.assertIn("InitializeAndroidThreadAffinity()", host_query)
        self.assertIn("topology", THREAD_CPP)
        self.assertIn("snapdragon_8_gen_3", host_query)
        self.assertIn("bool IsSnapdragon8Gen3Host();", THREAD_H)
        self.assertIn("bool IsSnapdragon8Gen5Host();", THREAD_H)
        self.assertIn('return normalized == "sm8850";', THREAD_CPP)
        sm8850_query = function_body(THREAD_CPP, "bool IsSnapdragon8Gen5Host()")
        self.assertIn("snapdragon_8_gen_5", sm8850_query)

        self.assertIn(
            "const bool sm8650_software_tso_memory_path =", NATIVE_RUNTIME
        )
        self.assertIn("Common::IsSnapdragon8Gen3Host()", NATIVE_RUNTIME)
        self.assertIn(
            "LSX4_FEX_HOST_OS_HARDWARE_TSO_ENABLED", NATIVE_RUNTIME
        )
        self.assertIn(
            'setenv("EXECUTOR_FEX_DEFERRED_BULK_GUEST_WRITES", "1", 1)',
            NATIVE_RUNTIME,
        )
        self.assertIn(
            'unsetenv("EXECUTOR_FEX_DEFERRED_BULK_GUEST_WRITES")',
            NATIVE_RUNTIME,
        )
        self.assertIn(
            '"run-enable-fex-deferred-bulk-guest-writes"',
            NATIVE_RUNTIME,
        )
        synchronous_policy = NATIVE_RUNTIME.split(
            "const bool sm8650_synchronous_page_manager_requested =", 1
        )[1].split(";", 1)[0]
        self.assertIn("sm8650_software_tso_memory_path", synchronous_policy)
        self.assertNotIn("std::filesystem::exists", synchronous_policy)
        write_epoch_policy = NATIVE_RUNTIME.split(
            "const bool sm8650_synchronous_page_manager_requested =", 1
        )[1].split("#ifdef __ANDROID__", 1)[0]
        self.assertIn(
            "!sm8650_synchronous_page_manager_requested",
            write_epoch_policy,
        )
        deferred_epoch_policy = NATIVE_RUNTIME.split(
            "const bool deferred_write_epoch_requested =", 1
        )[1].split(
            "const bool deferred_write_epoch_enabled =", 1
        )[0]
        self.assertIn(
            "!sm8650_synchronous_page_manager_requested",
            deferred_epoch_policy,
        )
        bulk_policy = NATIVE_RUNTIME.split(
            "const bool deferred_bulk_guest_writes_requested =", 1
        )[1].split("NativeLog(ANDROID_LOG_INFO", 1)[0]
        self.assertIn("sm8650_software_tso_memory_path", bulk_policy)
        self.assertIn("deferred_write_epoch_enabled", bulk_policy)
        self.assertIn("deferred_bulk_guest_writes_requested", bulk_policy)
        self.assertNotIn("LSX4_FEX_CORTEX_X4_STATIC_L1_AB", NATIVE_RUNTIME)
        self.assertNotIn("LSX4_FEX_CORTEX_X4_MULTIBLOCK_AB", NATIVE_RUNTIME)

    def test_sm8650_deferred_epoch_ab_is_marker_only_and_default_off(self):
        policy = NATIVE_RUNTIME.split(
            "const bool sm8650_deferred_write_epoch_ab_requested =", 1
        )[1].split("#ifdef __ANDROID__", 1)[0]
        self.assertIn("sm8650_software_tso_memory_path", policy)
        self.assertIn(
            '"run-enable-sm8650-fex-deferred-write-epoch-ab"', policy
        )
        self.assertIn(
            "!sm8650_deferred_write_epoch_ab_requested", policy
        )
        self.assertIn("!sm8650_synchronous_page_manager_requested", policy)
        self.assertIn("sm8650DeferredEpochAb=%d", NATIVE_RUNTIME)

    def test_semantic_codegen_features_are_exact_sm8650_only(self):
        for feature in ("FCMA", "FRINTTS", "ECV"):
            self.assertIn(f"HWCAP{'2' if feature != 'FCMA' else ''}_{feature if feature != 'FRINTTS' else 'FRINT'}", HOST_ISA_PROFILE)
            self.assertIn(f"LSX4_FEX_HOST_ISA_{feature}", NATIVE_RUNTIME)

        policy = NATIVE_RUNTIME.split(
            "constexpr std::uint64_t sm8650_extended_fex_isa =", 1
        )[1].split("const bool relax_vector_tso_ab", 1)[0]
        self.assertIn("Common::IsSnapdragon8Gen3Host()", policy)
        self.assertIn('"run-enable-sm8650-fex-extended-isa-ab"', policy)
        self.assertIn("sm8650_extended_fex_isa_requested", policy)
        self.assertIn(
            "Common::IsSnapdragon8Gen3Host() && sm8650_extended_fex_isa_requested",
            policy,
        )
        self.assertIn("!sm8650_extended_fex_isa_enabled", policy)
        self.assertIn(
            "preflight.host_isa_feature_mask &= ~sm8650_extended_fex_isa",
            policy,
        )

    def test_strict_split_lock_ab_is_exact_sm8650_software_tso_only(self):
        policy = NATIVE_RUNTIME.split(
            "const bool sm8650_strict_split_locks_requested =", 1
        )[1].split("#else", 1)[0]
        self.assertIn("sm8650_software_tso_memory_path", policy)
        self.assertIn('"run-sm8650-fex-strict-split-locks-ab"', policy)
        self.assertIn(
            '::setenv("LSX4_FEX_STRICT_SPLIT_LOCKS", "1", 1)', policy
        )
        self.assertIn(
            '::unsetenv("LSX4_FEX_STRICT_SPLIT_LOCKS")', policy
        )

    def test_watcher_pte_transition_ab_is_exact_sm8650_only(self):
        policy = NATIVE_RUNTIME.split(
            "const bool sm8650_force_watcher_pte_transitions =", 1
        )[1].split(
            "const auto guest_invalidation_source_profile_sentinel", 1
        )[0]
        self.assertIn("Common::IsSnapdragon8Gen3Host()", policy)
        self.assertIn(
            '"run-sm8650-force-watcher-pte-transitions-ab"', NATIVE_RUNTIME
        )
        self.assertIn(
            'setenv("EXECUTOR_SM8650_FORCE_WATCHER_PTE_TRANSITIONS", "1", 1)',
            policy,
        )
        self.assertIn(
            'unsetenv("EXECUTOR_SM8650_FORCE_WATCHER_PTE_TRANSITIONS")',
            policy,
        )

        gate = function_body(
            PAGE_MANAGER,
            "bool ExecutorSm8650ForceWatcherPteTransitionsEnabled()",
        )
        self.assertIn("Common::IsSnapdragon8Gen3Host()", gate)
        self.assertIn(
            'std::getenv("EXECUTOR_SM8650_FORCE_WATCHER_PTE_TRANSITIONS")',
            gate,
        )
        apply = function_body(PAGE_MANAGER, "void ApplyWatcherProtection(")
        self.assertIn(
            "ExecutorSm8650ForceWatcherPteTransitionsEnabled()", apply
        )

    def test_sm8650_keeps_gpu_dirty_srt_validation_without_changing_gen5(self):
        self.assertIn(
            "const bool sm8650_requires_gpu_dirty_srt_validation =",
            NATIVE_RUNTIME,
        )
        self.assertIn("Common::IsSnapdragon8Gen3Host()", NATIVE_RUNTIME)
        srt_policy = NATIVE_RUNTIME.split(
            "const bool sm8650_requires_gpu_dirty_srt_validation =", 1
        )[1].split("const auto srt_cache_trace_sentinel", 1)[0]
        self.assertIn("!sm8650_requires_gpu_dirty_srt_validation", srt_policy)
        self.assertIn(
            'unsetenv("EXECUTOR_IMMUTABLE_SRT_SNAPSHOT_CACHE")', srt_policy
        )
        self.assertIn("gpu-dirty-srt-required", srt_policy)

    def test_deferred_epoch_owner_kick_is_exact_sm8650_software_tso_only(self):
        kick = function_body(
            NATIVE_RUNTIME,
            "executor_lsx4_android_fex_deferred_write_epoch_kick_owner(",
        )
        self.assertIn("FaultSignalOwner::Fex", kick)
        self.assertIn("Common::IsSnapdragon8Gen3Host()", kick)
        self.assertIn("LSX4_FEX_HOST_OS_HARDWARE_TSO_ENABLED", kick)
        self.assertIn("KickDeferredWriteEpochOwner", kick)

    def test_primary_call_uses_distinct_policy_without_changing_pthread_creation(self):
        primary = function_body(
            NATIVE_RUNTIME, "void PreferPerformanceCpusForPrimaryGuest()"
        )
        self.assertIn("ThreadAffinityClass::PrimaryGuest", primary)

        create_default = function_body(
            NATIVE_THREAD, "int NativeThread::CreateDefaultStack("
        )
        self.assertIn(
            "pthread_create(pthr, nullptr, (PthreadFunc)func, arg)",
            create_default,
        )
        self.assertNotIn("pthread_attr_setaffinity_np", create_default)

    def test_affinity_success_requires_exact_requested_mask(self):
        apply_body = function_body(
            THREAD_CPP, "bool ApplyCurrentThreadAffinity("
        )
        self.assertIn("EqualCpus(actual, expected)", apply_body)
        self.assertIn(
            "return result == 0 && !fallback_applied && actual_read",
            apply_body,
        )
        self.assertIn("EqualCpus(actual, requested)", apply_body)
        self.assertIn("requestedMask=0x%llx", apply_body)
        self.assertIn("actualMask=0x%llx", apply_body)
        self.assertIn("exact=%d", apply_body)
        self.assertNotIn("return result == 0 && actual_count != 0", apply_body)


if __name__ == "__main__":
    unittest.main()
