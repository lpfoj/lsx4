from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (
    ROOT
    / "funnel-arm"
    / "src"
    / "video_core"
    / "renderer_vulkan"
    / "vk_pipeline_cache.cpp"
).read_text(encoding="utf-8")


class Sm8650ProgramHotAndroidStackContract(unittest.TestCase):
    def test_structural_and_payload_defaults_are_published_separately(self):
        constructor = SOURCE[
            SOURCE.index("PipelineCache::PipelineCache(") :
            SOURCE.index("const char* const hle_clean_buffer_guard_flag")
        ]
        self.assertIn("capability_profile.exact_mesa_turnip_a750_d9_lineage", constructor)
        self.assertIn(
            "executor_exact_snapdragon_turnip_program_hot_structural_default.store(",
            constructor,
        )
        self.assertIn(
            "executor_exact_sm8650_turnip_d9_program_hot_payload_default.store(",
            constructor,
        )
        self.assertIn("capability_profile.exact_mesa_turnip_adreno840", constructor)
        self.assertIn(
            "exact_snapdragon_turnip_structural =\n"
            "        exact_sm8650_turnip_d9 || exact_sm8850_turnip",
            constructor,
        )
        self.assertIn(
            "exact_sm8650_turnip_d9 && !preserve_strong_snapdragon_bundle",
            constructor,
        )
        self.assertNotIn("exact_proprietary_adreno750", constructor)

    def test_structural_routes_cover_a840_but_payload_route_remains_a750_only(self):
        for marker in (
            "run-enable-program-hot-occupancy-mask",
            "run-enable-program-hot-grouped-tls",
            "run-enable-program-hot-call-scope-tls-hoist",
            "run-enable-program-hot-legacy-tls-cold-split",
        ):
            marker_at = SOURCE.index(marker)
            route = SOURCE[marker_at : marker_at + 420]
            self.assertIn(
                "ExecutorExactSnapdragonTurnipProgramHotStructuralDefaultEnabled()",
                route,
            )
        marker_at = SOURCE.index("run-enable-borrowed-fetch-shader-result")
        route = SOURCE[marker_at : marker_at + 420]
        self.assertIn(
            "ExecutorExactSm8650TurnipD9ProgramHotPayloadDefaultEnabled()", route
        )

    def test_default_read_is_cached_outside_the_hot_loop(self):
        self.assertGreaterEqual(SOURCE.count("static const bool enabled = []"), 5)
        self.assertIn("std::memory_order_acquire", SOURCE)
        self.assertIn("std::memory_order_release", SOURCE)

    def test_grouped_tls_dependencies_and_legacy_fallback_remain(self):
        self.assertIn(
            "ExecutorProgramHotGroupedTlsEnabled() &&\n"
            "        ExecutorProgramHotCallScopeTlsHoistEnabled()",
            SOURCE,
        )
        self.assertIn(
            "if (program_hot_grouped_tls_enabled &&\n"
            "        program_hot_legacy_tls_cold_split_enabled)",
            SOURCE,
        )
        self.assertIn("ExecutorProgramHotLegacyThreadStateRefs()", SOURCE)

    def test_occupancy_keeps_valid_mask_and_exact_candidate_proofs(self):
        self.assertIn("program_hot_valid_way_masks", SOURCE)
        self.assertIn("candidate.owner == this", SOURCE)
        self.assertIn("candidate.program == program.get()", SOURCE)
        self.assertIn("candidate.snapshot_key == snapshot_key", SOURCE)
        self.assertIn("candidate.runtime_info_identity == runtime_info_identity", SOURCE)
        self.assertIn("DoSrtSourceBytesMatch(candidate.srt_ranges,", SOURCE)

    def test_borrowed_fetch_keeps_owned_fallback_and_module_bounds(self):
        self.assertIn("ASSERT(module_index < result_program.modules.size())", SOURCE)
        self.assertIn("result.fetch_shader_program = &result_program", SOURCE)
        self.assertIn("result.owned_fetch_shader =", SOURCE)
        self.assertIn("result_program.modules[module_index].spec.fetch_shader_data", SOURCE)

    def test_runtime_marker_names_the_exact_stack(self):
        self.assertIn("[EXECUTOR_SNAPDRAGON_PROGRAM_HOT_ANDROID_STACK] enabled=1", SOURCE)
        self.assertIn("exactA750D9=%u exactA840Turnip=%u occupancyMask=1 groupedTls=1", SOURCE)
        self.assertIn("borrowedFetchResult=%u", SOURCE)
        self.assertIn("manualMarkersPreserved=1", SOURCE)


if __name__ == "__main__":
    unittest.main()
