from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (
    ROOT / "funnel-arm/src/video_core/renderer_vulkan/vk_pipeline_cache.cpp"
).read_text(encoding="utf-8")
HEADER = (
    ROOT / "funnel-arm/src/video_core/renderer_vulkan/vk_pipeline_cache.h"
).read_text(encoding="utf-8")


class ProgramHotCallScopeTlsHoistContractTests(unittest.TestCase):
    def test_candidate_is_default_off_and_requires_grouped_tls(self):
        self.assertIn("run-enable-program-hot-call-scope-tls-hoist", SOURCE)
        refresh = SOURCE.split("PipelineCache::RefreshGraphicsStages", 1)[1].split(
            "PipelineCache::RefreshComputeKey", 1
        )[0]
        self.assertIn("ExecutorProgramHotGroupedTlsEnabled() &&", refresh)
        self.assertIn("ExecutorProgramHotCallScopeTlsHoistEnabled()", refresh)

    def test_one_call_scope_state_is_passed_to_each_program_lookup(self):
        self.assertIn("struct ProgramHotThreadState;", HEADER)
        self.assertIn("ProgramHotThreadState* call_scope_program_hot_state", HEADER)
        refresh = SOURCE.split("PipelineCache::RefreshGraphicsStages", 1)[1].split(
            "PipelineCache::RefreshComputeKey", 1
        )[0]
        self.assertEqual(refresh.count("&ExecutorProgramHotThreadState()"), 1)
        self.assertRegex(
            refresh,
            re.compile(
                r"refresh_runtime_info,\s+call_scope_program_hot_state"
            ),
        )

    def test_get_program_keeps_the_old_fallback(self):
        get_program = SOURCE.split("PipelineCache::GetProgram(", 1)[1]
        self.assertIn(
            "if (program_hot_grouped_tls_enabled && grouped_state == nullptr)",
            get_program,
        )
        self.assertIn("grouped_state = &ExecutorProgramHotThreadState()", get_program)
        self.assertIn("ExecutorProgramHotLegacyThreadStateRefs()", get_program)

    def test_cache_payload_and_identity_proofs_are_not_changed(self):
        for field in (
            "ProgramHotSets sets",
            "ProgramHotByteArray mru_ways",
            "ProgramHotByteArray valid_way_masks",
            "u64 exact_revalidations",
            "u64 retained_content_versions",
        ):
            self.assertIn(field, SOURCE)
        self.assertIn("cacheFieldsUnchanged=1 proofsUnchanged=1", SOURCE)

    def test_runtime_witness_requires_two_program_calls(self):
        self.assertIn("++call_scope_program_calls == 2", SOURCE)
        self.assertIn("programCalls=2 tlsResolutionsSaved=1", SOURCE)
        self.assertIn("program-hot-call-scope-tls-hoist.txt", SOURCE)


if __name__ == "__main__":
    unittest.main()
