from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
PIPELINE = (
    ROOT / "funnel-arm/src/video_core/renderer_vulkan/vk_pipeline_cache.cpp"
).read_text(encoding="utf-8")


class A840InsideShaderPayloadRegressionContract(unittest.TestCase):
    def test_a840_is_not_an_automatic_shader_payload_reuse_route(self):
        constructor = PIPELINE[
            PIPELINE.index("PipelineCache::PipelineCache(") :
            PIPELINE.index("const char* const hle_clean_buffer_guard_flag")
        ]
        self.assertIn("capability_profile.exact_mesa_turnip_adreno840", constructor)
        self.assertIn(
            "executor_exact_sm8650_turnip_d9_program_hot_payload_default.store(\n"
            "        exact_sm8650_turnip_d9 && !preserve_strong_snapdragon_bundle",
            constructor,
        )
        self.assertNotIn(
            "executor_exact_sm8650_turnip_d9_program_hot_payload_default.store(\n"
            "        exact_snapdragon_turnip_structural",
            constructor,
        )

    def test_payload_reuse_routes_use_the_a750_default(self):
        for marker in (
            "run-enable-program-hot-flattened-lease",
            "run-enable-program-flattened-scratch-recycle",
            "run-enable-borrowed-fetch-shader-result",
        ):
            marker_at = PIPELINE.index(marker)
            route = PIPELINE[marker_at : marker_at + 440]
            self.assertIn(
                "ExecutorExactSm8650TurnipD9ProgramHotPayloadDefaultEnabled()",
                route,
            )

    def test_a840_keeps_state_neutral_program_hot_acceleration(self):
        for marker in (
            "run-enable-program-hot-occupancy-mask",
            "run-enable-program-hot-grouped-tls",
            "run-enable-program-hot-call-scope-tls-hoist",
            "run-enable-program-hot-legacy-tls-cold-split",
        ):
            marker_at = PIPELINE.index(marker)
            route = PIPELINE[marker_at : marker_at + 440]
            self.assertIn(
                "ExecutorExactSnapdragonTurnipProgramHotStructuralDefaultEnabled()",
                route,
            )


if __name__ == "__main__":
    unittest.main()
