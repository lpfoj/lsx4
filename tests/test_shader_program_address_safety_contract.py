import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
PIPELINE = (
    ROOT
    / "funnel-arm"
    / "src"
    / "video_core"
    / "renderer_vulkan"
    / "vk_pipeline_cache.cpp"
).read_text(encoding="utf-8")


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


class ShaderProgramAddressSafetyContractTest(unittest.TestCase):
    def test_owned_range_is_checked_before_the_first_raw_shader_load(self):
        get_params = cpp_function(
            PIPELINE,
            "std::optional<Shader::ShaderParams> PipelineCache::GetShaderParams",
        )

        floor_guard = get_params.index("address < guest_address_floor")
        aperture_guard = get_params.index("IsValidGpuMapping")
        raw_scan = get_params.index("AmdGpu::TrySearchBinaryInfo(code)")
        self.assertLess(floor_guard, raw_scan)
        self.assertLess(aperture_guard, raw_scan)
        self.assertIn("AmdGpu::ShaderBinaryProbeBytes", get_params[:raw_scan])
        self.assertIn("[EXECUTOR_SHADER_ADDRESS_RETRY]", get_params[:raw_scan])
        self.assertIn("return std::nullopt", get_params[floor_guard:raw_scan])
        self.assertNotIn("ContainsOwnedRange", get_params[:raw_scan])

    def test_address_guard_has_no_gpu_synchronization_or_mapping_lock(self):
        get_params = cpp_function(
            PIPELINE,
            "std::optional<Shader::ShaderParams> PipelineCache::GetShaderParams",
        )
        guard_path = get_params[: get_params.index("const auto scan_metadata")]

        for forbidden in (
            "WaitIdle",
            "Finish",
            "ResolveGpuDirtyMemoryForCpuRead",
            "TryCopyReadableMemory",
            "TryCopyBackingMemory",
            "IsReadableMapping",
            "AcquireSnapshotReadGuard",
        ):
            self.assertNotIn(forbidden, guard_path)

    def test_invalid_graphics_stage_retries_instead_of_reusing_stale_state(self):
        refresh = cpp_function(
            PIPELINE, "bool PipelineCache::RefreshGraphicsStages"
        )
        bind_start = refresh.index("const auto bind_stage")
        bind_end = refresh.index("infos.fill(nullptr)", bind_start)
        bind_stage = refresh[bind_start:bind_end]

        params_call = bind_stage.index("GetShaderParams(")
        params_miss = bind_stage.index("if (!params)", params_call)
        self.assertIn("ExecutorGraphicsShaderBindOutcome::ParamsRetry", bind_stage[params_miss:])
        self.assertIn("return StageBindResult::Retry", bind_stage[params_miss:])

    def test_compute_uses_the_same_guarded_shader_parameter_path(self):
        compute = cpp_function(PIPELINE, "bool PipelineCache::RefreshComputeKey")

        self.assertIn("GetShaderParams(", compute)
        self.assertIn("if (!cs_params)", compute)
        self.assertIn("return false", compute[compute.index("if (!cs_params)") :])


if __name__ == "__main__":
    unittest.main()
