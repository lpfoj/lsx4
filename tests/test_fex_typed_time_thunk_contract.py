import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
IDENTITY = (ROOT / "src/executor/dynamic_translation/hle_thunk_identity.h").read_text(
    encoding="utf-8"
)
STUBS = (ROOT / "funnel-arm/src/core/aerolib/stubs.cpp").read_text(
    encoding="utf-8"
)
ABI = (ROOT / "src/executor/fex_backend_abi_v3.h").read_text(encoding="utf-8")
RUNTIME = (ROOT / "src/executor/native_runtime_api.cpp").read_text(encoding="utf-8")


class FexTypedTimeThunkContractTests(unittest.TestCase):
    def test_typed_identity_is_derived_from_negotiated_base(self):
        self.assertIn("DeriveFexTypedThunkIdentity", IDENTITY)
        for delta in ("identity[0] ^= 0xa7", "identity[11] ^= 0x5d",
                      "identity[23] ^= 0xc3", "identity[31] ^= 0x6e"):
            self.assertIn(delta, IDENTITY)
        self.assertIn(
            "DeriveFexTypedThunkIdentity(FexThunkIdentity)", IDENTITY
        )

    def test_only_exact_time_imports_receive_typed_identity(self):
        route = STUBS.split("bool UseFexTypedTimeThunk", 1)[1].split(
            "u64 GetAndroidX64HleStub", 1
        )[0]
        for target in (
            "posix_gettimeofday",
            "n88vx3C5nW8",
            "sceKernelGettimeofday",
            "ejekcaNQNq0",
        ):
            self.assertIn(target, route)
        for forbidden in ("memcpy", "memset", "mutex", "semaphore", "gnm"):
            self.assertNotIn(forbidden, route.lower())

    def test_jit_and_general_fex_routes_are_unchanged(self):
        emission = STUBS.split("const auto& hle_hash = ExecutorJitActiveForStubGeneration()", 1)[1]
        emission = emission.split("std::memcpy(code + cursor, hle_hash.data()", 1)[0]
        self.assertIn("TranslationThunkIdentity", emission)
        self.assertIn("FexTypedThunkIdentity", emission)
        self.assertIn("FexThunkIdentity", emission)

    def test_exact_typed_time_callback_is_append_only_and_fail_closed(self):
        self.assertIn("try_dispatch_typed_time_hle", ABI)
        self.assertIn("try_dispatch_typed_time_hle, 72", ABI)
        metadata_build = RUNTIME.split(
            "ExecutorHleTargetMetadata BuildExecutorHleTargetMetadata", 1
        )[1].split("metadata.jit_leaf_direct_safe", 1)[0]
        callback = RUNTIME.split("int32_t ExecutorFexTryDispatchTypedTimeHle(", 1)[1]
        callback = callback.split("const Lsx4FexHostCallbacksV3&", 1)[0]
        for exact in (
            "sceKernelGettimeofday",
            "ejekcaNQNq0",
            "posix_gettimeofday",
            "n88vx3C5nW8",
        ):
            self.assertIn(exact, metadata_build)
        self.assertIn("metadata.exact_typed_time_target", callback)
        self.assertNotIn("HleNameMatches", callback)
        for required in (
            "ScopedFexHleDispatchDepth",
            "ScopedFexBulkLeaseAbort",
            "ScopedExecutorLeafHleBoundary",
            "metadata.fex_guest_thread_exit_target",
            "metadata.fex_context_replacing_fiber_target",
        ):
            self.assertIn(required, callback)
        self.assertNotIn("SymbolNameContains", callback)

        gate = RUNTIME.split("bool FexTypedTimeDirectHleDispatchEnabled()", 1)[1]
        gate = gate.split("using box64_cstr_fn", 1)[0]
        self.assertIn("run-disable-fex-typed-time-direct", gate)
        callbacks = RUNTIME.split("const Lsx4FexHostCallbacksV3& ExecutorFexHostCallbacks", 1)[1]
        callbacks = callbacks.split("return callbacks", 1)[0]
        self.assertIn("FexTypedTimeDirectHleDispatchEnabled()", callbacks)
        self.assertIn(": nullptr", callbacks)


if __name__ == "__main__":
    unittest.main()
