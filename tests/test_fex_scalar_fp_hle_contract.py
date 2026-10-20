from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
ABI = (ROOT / "src/executor/fex_backend_abi_v3.h").read_text(encoding="utf-8")
RUNTIME = (ROOT / "src/executor/native_runtime_api.cpp").read_text(
    encoding="utf-8"
)


def test_scalar_fp_callback_is_append_only_and_optional():
    assert "Lsx4FexTryScalarFpHleDispatchCallbackV3" in ABI
    assert "try_dispatch_scalar_fp_hle" in ABI
    assert "LSX4_FEX_ABI_ASSERT_POD(Lsx4FexHostCallbacksV3, 88)" in ABI
    assert "try_dispatch_leaf_hle, 64" in ABI
    assert "try_dispatch_typed_time_hle, 72" in ABI
    assert "try_dispatch_scalar_fp_hle, 80" in ABI

    callbacks = RUNTIME.split("ExecutorFexHostCallbacks()", 1)[1].split(
        "return callbacks", 1
    )[0]
    assert "FexDirectScalarFpHleDispatchEnabled()" in callbacks
    assert "ExecutorFexTryDispatchScalarFpHle" in callbacks
    assert ": nullptr" in callbacks


def test_scalar_fp_gate_is_snapdragon_title_scoped_and_fail_closed():
    gate = RUNTIME.split("bool FexDirectScalarFpHleDispatchEnabled()", 1)[1].split(
        "bool FexTypedTimeDirectHleDispatchEnabled()", 1
    )[0]
    assert "Common::IsSnapdragon8Gen3Host()" in gate
    assert "Common::IsSnapdragon8Gen5Host()" in gate
    assert "run-enable-sm8650-fex-direct-scalar-fp-hle" in gate
    assert "run-enable-sm8850-fex-direct-scalar-fp-hle" in gate
    assert "ExecutorTitleScopedCpuOptimizationMarkerEnabled(marker)" in gate
    assert "CUSA" not in gate


def test_scalar_fp_provider_admits_only_exact_pure_scalar_abis():
    callback = RUNTIME.split("int32_t ExecutorFexTryDispatchScalarFpHle(", 1)[1]
    callback = callback.split(
        "const Lsx4FexHostCallbacksV3& ExecutorFexHostCallbacks", 1
    )[0]
    for admitted in (
        "ExecutorHleFpAbi::UnaryF32",
        "ExecutorHleFpAbi::BinaryF32",
        "ExecutorHleFpAbi::UnaryF64",
        "ExecutorHleFpAbi::BinaryF64",
    ):
        assert admitted in callback
    for rejected in (
        "ExecutorHleFpAbi::SincosF32",
        "ExecutorHleFpAbi::SincosF64",
        "ExecutorHleFpAbi::Ngs2PanInit",
        "ExecutorHleFpAbi::VideoOutGamma",
        "metadata.stack_abi != ExecutorHleStackAbi::None",
        "metadata.fex_guest_thread_exit_target",
        "metadata.fex_context_replacing_fiber_target",
    ):
        assert rejected in callback
    for boundary in (
        "ScopedFexHleDispatchDepth",
        "ScopedFexBulkLeaseAbort",
        "ScopedExecutorLeafHleBoundary",
        "RecordMappedEntryHleBoundary",
        "RecordMappedEntryHleResult",
    ):
        assert boundary in callback
    assert "HleNameMatches" not in callback
    assert "SymbolNameContains" not in callback
    assert "CUSA" not in callback
