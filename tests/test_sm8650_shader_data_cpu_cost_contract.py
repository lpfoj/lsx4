from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
PIPELINE = (
    ROOT
    / "funnel-arm/src/video_core/renderer_vulkan/vk_pipeline_cache.cpp"
).read_text(encoding="utf-8")
LIVERPOOL = (
    ROOT / "funnel-arm/src/video_core/amdgpu/liverpool.cpp"
).read_text(encoding="utf-8")
SRT = (
    ROOT
    / "funnel-arm/src/shader_recompiler/ir/passes/flatten_extended_userdata_pass.cpp"
).read_text(encoding="utf-8")


class Sm8650ShaderDataCpuCostContract(unittest.TestCase):
    def test_pipeline_routes_require_the_exact_d9_default(self):
        for marker in (
            "run-enable-program-hot-flattened-lease",
            "run-enable-program-flattened-scratch-recycle",
        ):
            marker_at = PIPELINE.index(marker)
            route = PIPELINE[marker_at : marker_at + 420]
            self.assertIn(
                "ExecutorExactSm8650TurnipD9ProgramHotPayloadDefaultEnabled()",
                route,
            )
        self.assertIn("exact_mesa_turnip_a750_d9_lineage", PIPELINE)
        self.assertIn("exact_mesa_turnip_adreno840", PIPELINE)

    def test_flattened_lease_keeps_owned_fallback(self):
        self.assertIn("program_hot_hit->flattened_user_data_lease", PIPELINE)
        self.assertIn("program->ActivateCachedPermutLease(", PIPELINE)
        self.assertIn("info.flattened_ud_lease.reset()", PIPELINE)
        self.assertIn("program->ActivatePermut(", PIPELINE)

    def test_scratch_recycle_keeps_default_path(self):
        self.assertIn("program_flattened_scratch_recycle_enabled", PIPELINE)
        self.assertIn("ExecutorRecordProgramFlattenedScratchRecycle()", PIPELINE)
        self.assertIn("ExecutorProgramFlattenedScratchRecycleEnabled()", PIPELINE)

    def test_gpu_thread_token_is_snapdragon_or_manual_only(self):
        gate_at = LIVERPOOL.index("bool ExecutorGpuThreadTokenEnabled()")
        gate = LIVERPOOL[gate_at : gate_at + 1200]
        self.assertIn("run-enable-gpu-thread-token", gate)
        self.assertIn("requested || Common::IsSnapdragon8Gen3Host() ||", gate)
        self.assertIn("Common::IsSnapdragon8Gen5Host()", gate)
        self.assertIn("__builtin_thread_pointer()", LIVERPOOL)
        self.assertIn("gpu_command_thread_token", LIVERPOOL)
        self.assertIn("gpu_command_thread_owner", LIVERPOOL)

    def test_srt_layout_and_default_init_are_snapdragon_or_manual_only(self):
        for marker in (
            "run-enable-portable-srt-precomputed-layout",
            "run-enable-portable-srt-scratch-default-init",
        ):
            marker_at = SRT.index(marker)
            route = SRT[marker_at : marker_at + 620]
            self.assertIn("Common::IsSnapdragon8Gen3Host()", route)
            self.assertIn("Common::IsSnapdragon8Gen5Host()", route)
        self.assertIn("#include \"common/thread.h\"", SRT)

    def test_precomputed_layout_retains_validation_and_legacy_walker(self):
        self.assertIn("if (use_precomputed_layout && !srt.portable_layout_valid)", SRT)
        self.assertIn("if (!use_precomputed_layout)", SRT)
        self.assertIn("legacy_node_span_dw", SRT)
        self.assertIn("legacy_node_bulk_offsets", SRT)
        self.assertIn("memoryReadsUnchanged=1", SRT)

    def test_default_init_writes_every_value_before_use(self):
        self.assertIn("bases[i] = 0", SRT)
        self.assertIn("node_bulk_valid[i] = 0", SRT)
        self.assertIn("boost::container::default_init", SRT)
        self.assertIn("bulkWordsOverwriteGuard=1", SRT)


if __name__ == "__main__":
    unittest.main()
