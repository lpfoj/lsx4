from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
HEADER = (
    ROOT
    / "funnel-arm"
    / "src"
    / "video_core"
    / "renderer_vulkan"
    / "vk_pipeline_cache.h"
).read_text(encoding="utf-8")
SOURCE = (
    ROOT
    / "funnel-arm"
    / "src"
    / "video_core"
    / "renderer_vulkan"
    / "vk_pipeline_cache.cpp"
).read_text(encoding="utf-8")


class BorrowedFetchShaderResultContractTests(unittest.TestCase):
    def test_candidate_is_default_off_and_marker_gated(self):
        self.assertIn("run-enable-borrowed-fetch-shader-result", SOURCE)
        self.assertIn("ExecutorBorrowedFetchShaderResultEnabled", SOURCE)
        self.assertIn("return false;", SOURCE.split(
            "constexpr bool ExecutorBorrowedFetchShaderResultEnabled", 1
        )[1].split("}", 1)[0])

    def test_result_keeps_owned_fallback_and_stable_program_index(self):
        result = HEADER.split("struct Result {", 1)[1].split("\n    };", 1)[0]
        self.assertIn("owned_fetch_shader", result)
        self.assertIn("const Program* fetch_shader_program", result)
        self.assertIn("fetch_shader_module_index", result)
        self.assertIn("borrowed_fetch_shader", result)
        self.assertIn("fetch_shader_program->modules", result)

    def test_all_get_program_results_use_the_single_gated_builder(self):
        get_program = SOURCE.split(
            "std::optional<PipelineCache::Result> PipelineCache::GetProgram", 1
        )[1]
        self.assertEqual(get_program.count("const auto make_result ="), 1)
        self.assertEqual(get_program.count("make_result("), 3)
        self.assertNotIn("std::make_tuple(&selected_info", get_program)

    def test_hot_cache_generation_and_identity_guards_remain(self):
        self.assertIn(
            "candidate.module_generation == program->module_generation", SOURCE
        )
        self.assertIn("candidate.program == program.get()", SOURCE)
        self.assertIn("program_hot.result = result", SOURCE)
        self.assertIn("resultCopySkipped=1", SOURCE)
        self.assertIn("ownedFallback=1", SOURCE)

    def test_borrowed_path_publishes_only_one_owned_pipeline_copy(self):
        refresh = SOURCE.split(
            "bool PipelineCache::RefreshGraphicsStages", 1
        )[1].split("bool PipelineCache::RefreshComputeKey", 1)[0]
        borrowed = refresh.split(
            "if (program_result.borrowed_fetch_shader)", 1
        )[1].split("} else {", 1)[0]
        self.assertIn("fetch_shader = *fetch_shader_data", borrowed)
        self.assertNotIn("std::optional<Shader::Gcn::FetchShaderData>", borrowed)
        self.assertIn("Preserve the pre-D84 two-step copy", refresh)


if __name__ == "__main__":
    unittest.main()
