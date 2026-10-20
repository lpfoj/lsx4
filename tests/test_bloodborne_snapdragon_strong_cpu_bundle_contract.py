from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
RUNTIME = (ROOT / "src/executor/native_runtime_api.cpp").read_text(encoding="utf-8")
PIPELINE = (
    ROOT / "funnel-arm/src/video_core/renderer_vulkan/vk_pipeline_cache.cpp"
).read_text(encoding="utf-8")
PAGE_MANAGER = (ROOT / "funnel-arm/src/video_core/page_manager.cpp").read_text(
    encoding="utf-8"
)
LIVERPOOL = (ROOT / "funnel-arm/src/video_core/amdgpu/liverpool.cpp").read_text(
    encoding="utf-8"
)
SRT = (
    ROOT
    / "funnel-arm/src/shader_recompiler/ir/passes/flatten_extended_userdata_pass.cpp"
).read_text(encoding="utf-8")


class BloodborneSnapdragonStrongCpuBundleContract(unittest.TestCase):
    def test_bundle_is_exact_title_and_host_scoped(self):
        helper = RUNTIME.split(
            "bool ExecutorBloodborneSnapdragonStrongCpuBundleEligible() noexcept", 1
        )[1].split("bool ExecutorPreserveBloodborneSnapdragonStrongBundle", 1)[0]
        self.assertIn('g_guest_cpu_backend_launch_title_id == "CUSA03173"', helper)
        self.assertIn('g_guest_cpu_backend_hardware_profile == "adreno-750"', helper)
        self.assertIn('g_guest_cpu_backend_hardware_profile == "adreno-840"', helper)
        self.assertNotIn('"mali"', helper)

    def test_same_binary_umbrella_preserve_marker_covers_every_bundle_layer(self):
        marker = "run-preserve-bloodborne-snapdragon-strong-bundle"
        for source in (RUNTIME, PIPELINE, PAGE_MANAGER, LIVERPOOL, SRT):
            with self.subTest(source_length=len(source)):
                self.assertIn(marker, source)

    def test_cpu_and_render_layers_have_independent_preserve_markers(self):
        self.assertIn(
            "run-preserve-bloodborne-snapdragon-cpu-bundle", RUNTIME
        )
        self.assertIn(
            "!ExecutorPreserveBloodborneSnapdragonCpuBundle()", RUNTIME
        )
        render_marker = "run-preserve-bloodborne-snapdragon-render-bundle"
        for source in (PIPELINE, PAGE_MANAGER, LIVERPOOL, SRT):
            with self.subTest(source_length=len(source)):
                self.assertIn(render_marker, source)
        self.assertGreaterEqual(
            PIPELINE.count("ExecutorPreserveBloodborneSnapdragonRenderBundle()"),
            9,
        )

    def test_automatic_cpu_bundle_is_a840_only_after_a750_liveness_rejection(self):
        default = RUNTIME.split(
            "bool ExecutorBloodborneSnapdragonStrongCpuBundleDefault() noexcept", 1
        )[1].split("std::string g_guest_cpu_backend_status", 1)[0]
        self.assertIn(
            'g_guest_cpu_backend_hardware_profile == "adreno-840"', default
        )
        self.assertNotIn('g_guest_cpu_backend_hardware_profile == "adreno-750"', default)

    def test_d51_and_d98_share_the_same_default(self):
        setup = RUNTIME.split("const auto fast_bulk_guest_write_sentinel", 1)[1]
        setup = setup.split("const auto immutable_snapshot_plan_sentinel", 1)[0]
        self.assertIn("ExecutorBloodborneSnapdragonStrongCpuBundleDefault()", setup)
        self.assertIn('setenv("EXECUTOR_FAST_BULK_GUEST_WRITE", "1", 1)', setup)
        self.assertIn(
            'setenv("EXECUTOR_FUSED_FAST_BULK_GUEST_WRITE_LEASE", "1", 1)',
            setup,
        )
        self.assertIn('unsetenv("EXECUTOR_FAST_BULK_GUEST_WRITE")', setup)
        self.assertIn(
            'unsetenv("EXECUTOR_FUSED_FAST_BULK_GUEST_WRITE_LEASE")', setup
        )

    def test_d53_keeps_manual_override_and_full_fallback(self):
        gate = RUNTIME.split("bool FexLeafHleBoundaryElisionEnabled()", 1)[1]
        gate = gate.split("bool FexDirectLeafHleDispatchEnabled()", 1)[0]
        self.assertIn("run-enable-fex-leaf-hle-boundary-elision", gate)
        self.assertIn("ExecutorBloodborneSnapdragonStrongCpuBundleDefault()", gate)
        self.assertIn("return true;", gate)

    def test_explicit_cpu_markers_override_only_the_automatic_preserve(self):
        gate = RUNTIME.split("bool FexLeafHleBoundaryElisionEnabled()", 1)[1]
        gate = gate.split("bool FexDirectLeafHleDispatchEnabled()", 1)[0]
        self.assertLess(
            gate.index("run-enable-fex-leaf-hle-boundary-elision"),
            gate.index("ExecutorPreserveBloodborneSnapdragonCpuBundle()"),
        )
        setup = RUNTIME.split("const auto fast_bulk_guest_write_sentinel", 1)[1]
        setup = setup.split("const auto immutable_snapshot_plan_sentinel", 1)[0]
        self.assertIn(
            "fast_bulk_guest_write_requested || strong_snapdragon_cpu_bundle",
            setup,
        )
        self.assertIn(
            "fused_fast_bulk_guest_write_lease_requested ||\n"
            "        strong_snapdragon_cpu_bundle",
            setup,
        )


if __name__ == "__main__":
    unittest.main()
