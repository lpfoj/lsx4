from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
ABI = (ROOT / "src/executor/fex_backend_abi_v3.h").read_text(encoding="utf-8")
RUNTIME = (ROOT / "src/executor/native_runtime_api.cpp").read_text(encoding="utf-8")
PERFORMANCE = (
    ROOT / "src/executor/android_performance_hint.cpp"
).read_text(encoding="utf-8")


def test_direct_leaf_callback_is_append_only_and_optional():
    assert "Lsx4FexTryLeafHleDispatchCallbackV3" in ABI
    assert "Lsx4FexTryLeafHleDispatchCallbackV3 try_dispatch_leaf_hle" in ABI
    assert "LSX4_FEX_ABI_ASSERT_POD(Lsx4FexHostCallbacksV3, 88)" in ABI
    assert "try_dispatch_leaf_hle, 64" in ABI
    assert "try_dispatch_typed_time_hle, 72" in ABI
    assert "try_dispatch_scalar_fp_hle, 80" in ABI
    callbacks = RUNTIME.split("ExecutorFexHostCallbacks()", 1)[1].split(
        "return callbacks", 1
    )[0]
    assert "FexDirectLeafHleDispatchEnabled()" in callbacks
    assert ": nullptr" in callbacks


def test_direct_leaf_admission_is_exact_sm8650_title_marker_gated_and_fail_closed():
    gate = RUNTIME.split("bool FexDirectLeafHleDispatchEnabled()", 1)[1].split(
        "using box64_cstr_fn", 1
    )[0]
    assert "Common::IsSnapdragon8Gen3Host()" in gate
    assert "run-enable-sm8650-fex-direct-leaf-hle" in gate
    assert "ExecutorTitleScopedCpuOptimizationMarkerEnabled(marker)" in gate
    direct = RUNTIME.split("int32_t ExecutorFexTryDispatchLeafHle(", 1)[1].split(
        "const Lsx4FexHostCallbacksV3& ExecutorFexHostCallbacks", 1
    )[0]
    for required in (
        "metadata.jit_leaf_direct_safe",
        "metadata.fp_abi != ExecutorHleFpAbi::None",
        "metadata.stack_abi != ExecutorHleStackAbi::None",
        "metadata.fex_guest_thread_exit_target",
        "metadata.fex_context_replacing_fiber_target",
        "ScopedFexHleDispatchDepth",
        "ScopedFexBulkLeaseAbort",
        "ScopedExecutorLeafHleBoundary",
        "RecordMappedEntryHleBoundary",
        "RecordMappedEntryHleResult",
    ):
        assert required in direct



    assert "std::atomic_thread_fence" not in direct
    assert "PublishCurrentJitGcRegisterRootsAtHleBoundary" not in direct
    prelude = RUNTIME.split("struct ExecutorHleBoundaryPrelude", 1)[1].split(
        "struct ScopedExecutorHleGuestReadLease", 1
    )[0]
    leaf_boundary = RUNTIME.split("struct ScopedExecutorLeafHleBoundary", 1)[1].split(
        "enum class ExecutorHleFpAbi", 1
    )[0]
    assert "PublishCurrentJitGcRegisterRootsAtHleBoundary" in prelude
    assert prelude.count("std::atomic_thread_fence") == 1
    assert leaf_boundary.count("std::atomic_thread_fence") == 1


def test_direct_leaf_has_a_separate_exact_sm8850_opt_in_route():
    gate = RUNTIME.split("bool FexDirectLeafHleDispatchEnabled()", 1)[1].split(
        "using box64_cstr_fn", 1
    )[0]
    assert "Common::IsSnapdragon8Gen5Host()" in gate
    assert "run-enable-sm8850-fex-direct-leaf-hle" in gate
    assert "sm8650 ? Sm8650EnableMarker : Sm8850EnableMarker" in gate
    assert "ExecutorTitleScopedCpuOptimizationMarkerEnabled(marker)" in gate


def test_legacy_root_marker_is_fail_closed_to_verified_bloodborne_profile():
    helper = RUNTIME.split(
        "bool ExecutorTitleScopedCpuOptimizationMarkerEnabled", 1
    )[1].split("std::string g_guest_cpu_backend_status", 1)[0]
    assert "Common::TitleScopedOptimizationMarkerEnabled(" in helper
    assert "g_guest_cpu_backend_launch_title_id" in helper
    assert "if (!ExecutorBloodborneSnapdragonStrongCpuBundleEligible())" in helper


def test_critical_nice_stays_at_the_non_starving_default():
    assert "run-enable-sm8650-critical-thread-nice" not in PERFORMANCE
    assert "CriticalThreadNiceTarget" not in PERFORMANCE
    scheduling = PERFORMANCE.split("void ApplyCriticalThreadScheduling", 1)[1]
    assert "current_nice > -4" in scheduling
    assert "setpriority(PRIO_PROCESS, tid, -4)" in scheduling
    assert "niceTarget=%d" not in scheduling


def test_direct_leaf_success_has_no_per_hit_one_shot_log_atomic():
    direct = RUNTIME.split("int32_t ExecutorFexTryDispatchLeafHle(", 1)[1].split(
        "const Lsx4FexHostCallbacksV3& ExecutorFexHostCallbacks", 1
    )[0]
    assert "first_hit_logged" not in direct
    assert "[EXECUTOR_FEX_DIRECT_LEAF_HLE]" not in direct
