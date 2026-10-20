import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "src/executor/native_runtime_api.cpp").read_text(encoding="utf-8")


class FexLeafHleBoundaryElisionContractTests(unittest.TestCase):
    def test_fex_leaf_boundary_elision_keeps_marker_and_bloodborne_default(self):
        self.assertIn("run-enable-fex-leaf-hle-boundary-elision", SOURCE)
        self.assertIn(
            "ExecutorTitleScopedCpuOptimizationMarkerEnabled(Marker)", SOURCE
        )
        self.assertIn("ExecutorBloodborneSnapdragonStrongCpuBundleDefault()", SOURCE)
        self.assertIn('g_guest_cpu_backend_launch_title_id == "CUSA03173"', SOURCE)
        self.assertIn('g_guest_cpu_backend_hardware_profile == "adreno-750"', SOURCE)
        self.assertIn('g_guest_cpu_backend_hardware_profile == "adreno-840"', SOURCE)
        self.assertIn(
            "FexLeafHleBoundaryElisionEnabled() && hle_guest_thread != 0", SOURCE
        )

    def test_fex_leaf_boundary_elision_reuses_existing_safe_target_contract(self):
        self.assertIn(
            "target_metadata->executable && target_metadata->jit_leaf_direct_safe",
            SOURCE,
        )
        self.assertIn("target_metadata->fp_abi == ExecutorHleFpAbi::None", SOURCE)
        self.assertIn("target_metadata->stack_abi == ExecutorHleStackAbi::None", SOURCE)

    def test_fex_leaf_boundary_keeps_memory_ordering_and_full_fallback(self):
        leaf = SOURCE.index("struct ScopedExecutorLeafHleBoundary")
        dispatch = SOURCE.index('extern "C" int32_t ExecutorFexDispatchHle')
        self.assertGreaterEqual(
            SOURCE.index("ExecutorHleBoundaryPrelude prelude;", leaf, dispatch), leaf
        )
        self.assertGreaterEqual(
            SOURCE.index(
                "std::atomic_thread_fence(std::memory_order_release);", leaf, dispatch
            ),
            leaf,
        )
        self.assertGreaterEqual(
            SOURCE.index(
                "std::optional<ScopedExecutorHleBoundary> hle_boundary;", dispatch
            ),
            dispatch,
        )
        self.assertGreaterEqual(SOURCE.index("hle_boundary.emplace(", dispatch), dispatch)
        self.assertGreaterEqual(
            SOURCE.index("InvokeAndroidX64HleTypedTargetResolved(", dispatch), dispatch
        )


if __name__ == "__main__":
    unittest.main()
