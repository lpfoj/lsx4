import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
FUNNEL = ROOT / "funnel-arm" / "src"
PIPELINE_COMMON = (
    FUNNEL / "video_core" / "renderer_vulkan" / "vk_pipeline_common.h"
).read_text(encoding="utf-8")
PIPELINE_CACHE = (
    FUNNEL / "video_core" / "renderer_vulkan" / "vk_pipeline_cache.cpp"
).read_text(encoding="utf-8")
SHADER_INFO = (FUNNEL / "shader_recompiler" / "info.h").read_text(
    encoding="utf-8"
)


def cpp_function(source: str, signature: str) -> str:
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 0
    for position in range(brace, len(source)):
        token = source[position]
        if token == "{":
            depth += 1
        elif token == "}":
            depth -= 1
            if depth == 0:
                return source[start : position + 1]
    raise AssertionError(f"unterminated function: {signature}")


class PipelineWarmupRuntimeInfoContractTest(unittest.TestCase):
    def test_stage_rebind_is_only_a_conditional_pointer_update(self):
        rebind = cpp_function(PIPELINE_COMMON, "void RebindStageInfo")

        self.assertIn("current != info", rebind)
        self.assertIn("current = info", rebind)
        self.assertIn("std::array<const Shader::Info*", PIPELINE_COMMON)
        self.assertNotIn(") const noexcept", rebind.split("{", 1)[0])
        for forbidden in ("mutex", "wait", "WaitIdle", "Submit", "CUSA"):
            self.assertNotIn(forbidden, rebind)

    def test_compute_cache_hit_rebinds_after_live_info_refresh(self):
        lookup = cpp_function(
            PIPELINE_CACHE, "const ComputePipeline* PipelineCache::GetComputePipeline"
        )

        self.assertLess(lookup.index("RefreshComputeKey()"), lookup.index("RebindStageInfo"))
        self.assertIn("Shader::LogicalStage::Compute, infos[0]", lookup)
        self.assertLess(lookup.index("!pipeline->IsValid()"), lookup.index("RebindStageInfo"))

    def test_graphics_map_resolution_rebinds_every_logical_stage(self):
        lookup = cpp_function(
            PIPELINE_CACHE, "const GraphicsPipeline* PipelineCache::GetGraphicsPipeline"
        )

        rebind = lookup.rsplit("auto* pipeline = it->second.get();", 1)[1]
        self.assertIn("Shader::LogicalStage::Compute", rebind)
        self.assertIn("RebindStageInfo", rebind)
        self.assertIn("infos[stage]", rebind)

    def test_missing_runtime_data_is_not_silently_skipped(self):
        push_ud = cpp_function(SHADER_INFO, "void PushUd")

        self.assertIn("user_data[index]", push_ud)
        self.assertNotIn("user_data.empty", push_ud)
        self.assertNotIn("user_data.data()", push_ud)


if __name__ == "__main__":
    unittest.main()
