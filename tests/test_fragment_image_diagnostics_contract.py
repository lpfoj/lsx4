from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
SPIRV = ROOT / "funnel-arm" / "src" / "shader_recompiler" / "backend" / "spirv"
CONTEXT_GET_SET = (SPIRV / "emit_spirv_context_get_set.cpp").read_text(
    encoding="utf-8"
)
IMAGE = (SPIRV / "emit_spirv_image.cpp").read_text(encoding="utf-8")
PIPELINE_SERIALIZATION = (
    ROOT
    / "funnel-arm"
    / "src"
    / "video_core"
    / "renderer_vulkan"
    / "vk_pipeline_serialization.cpp"
).read_text(encoding="utf-8")
PIPELINE_CACHE = (
    ROOT
    / "funnel-arm"
    / "src"
    / "video_core"
    / "renderer_vulkan"
    / "vk_pipeline_cache.cpp"
).read_text(encoding="utf-8")
NATIVE_API = (ROOT / "src" / "executor" / "native_runtime_api.cpp").read_text(
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


class FragmentImageDiagnosticsContractTest(unittest.TestCase):
    def test_modes_are_marker_only_and_default_off(self):
        markers = {
            "run-diagnostic-fs-query-size": "EXECUTOR_DIAGNOSTIC_FS_QUERY_SIZE",
            "run-diagnostic-fs-texel-fetch": "EXECUTOR_DIAGNOSTIC_FS_TEXEL_FETCH",
        }
        for marker, environment in markers.items():
            self.assertIn(f' / "{marker}"', NATIVE_API)
            self.assertIn(f'setenv("{environment}", "1", 1)', NATIVE_API)
            self.assertIn(f'std::getenv("{environment}")', CONTEXT_GET_SET + IMAGE)

        for signature in (
            "bool DiagnosticFragmentImageQuerySize()",
            "bool DiagnosticFragmentImageTexelFetch(const EmitContext& ctx)",
        ):
            source = CONTEXT_GET_SET if signature.endswith("QuerySize()") else IMAGE
            gate = cpp_function(source, signature)
            self.assertIn("static const bool enabled", gate)
            self.assertIn("value != nullptr && value[0] == '1'", gate)

    def test_query_size_uses_the_exact_sampled_image_and_low_bytes(self):
        query = cpp_function(
            IMAGE,
            "Id CaptureDiagnosticFragmentImageQuerySize(EmitContext& ctx, u32 handle)",
        )
        self.assertIn("const u32 image_index = handle & 0xFFFF", query)
        self.assertIn("const auto& texture = ctx.images[image_index]", query)
        self.assertIn("LoadedSampledImage(ctx, image_index)", query)
        self.assertIn("OpImageQuerySizeLod", query)
        self.assertIn("OpImageQuerySize", query)
        self.assertIn("ctx.ConstU32(0xFFu)", query)
        self.assertIn("1.0f / 255.0f", query)
        self.assertIn("low_byte_normalized(width)", query)
        self.assertIn("low_byte_normalized(height)", query)
        self.assertIn("ctx.f32_zero_value", query)
        self.assertNotIn("LoadedSampler", query)
        self.assertNotIn("OpSampledImage", query)
        for forbidden in (
            "wait",
            "WaitIdle",
            "Submit",
            "GameSerial",
            "pgm_hash",
            "CUSA",
        ):
            self.assertNotIn(forbidden, query)

        sample = cpp_function(IMAGE, "Id EmitImageSampleImplicitLod(")
        coordinates = sample.index("FloatSampleCoordinates")
        capture = sample.index("CaptureDiagnosticFragmentImageQuerySize", coordinates)
        sampler = sample.index("LoadedSampler", capture)
        self.assertLess(coordinates, capture)
        self.assertLess(capture, sampler)

    def test_texel_fetch_scales_and_clamps_the_same_coordinates_without_sampler(self):
        texel = cpp_function(
            IMAGE,
            "Id EmitDiagnosticFragmentImageTexelFetch(EmitContext& ctx, u32 handle, Id coords)",
        )
        self.assertIn("const u32 image_index = handle & 0xFFFF", texel)
        self.assertIn("const auto& texture = ctx.images[image_index]", texel)
        self.assertIn("LoadedSampledImage(ctx, image_index)", texel)
        self.assertIn("OpImageQuerySizeLod", texel)
        self.assertIn("OpFMul", texel)
        self.assertIn("OpConvertFToS", texel)
        self.assertIn("OpSMin", texel)
        self.assertIn("OpSMax", texel)
        self.assertIn("OpImageFetch", texel)
        self.assertIn("ImageOperandsMask::Lod", texel)
        self.assertIn("ImageOperandsMask::Sample", texel)
        self.assertNotIn("LoadedSampler", texel)
        self.assertNotIn("OpSampledImage", texel)
        for forbidden in ("wait", "WaitIdle", "Submit", "GameSerial", "pgm_hash", "CUSA"):
            self.assertNotIn(forbidden, texel)

        sample = cpp_function(IMAGE, "Id EmitImageSampleImplicitLod(")
        coordinates = sample.index("FloatSampleCoordinates")
        fetch = sample.index("EmitDiagnosticFragmentImageTexelFetch", coordinates)
        sampler = sample.index("LoadedSampler", fetch)
        self.assertLess(coordinates, fetch)
        self.assertLess(fetch, sampler)

    def test_only_query_size_overrides_output_while_texel_fetch_preserves_shading(self):
        output = cpp_function(CONTEXT_GET_SET, "void EmitSetAttribute(")
        self.assertIn("if (DiagnosticFragmentImageQuerySize())", output)
        self.assertIn("GetDiagnosticFragmentImageQuerySize(ctx)", output)
        self.assertNotIn("DiagnosticFragmentImageTexelFetch", output)
        for forbidden in ("GameSerial", "pgm_hash", "CUSA"):
            self.assertNotIn(forbidden, output)

    def test_shader_and_host_pipeline_cache_identities_include_both_modes(self):
        for environment in (
            "EXECUTOR_DIAGNOSTIC_FS_QUERY_SIZE",
            "EXECUTOR_DIAGNOSTIC_FS_TEXEL_FETCH",
        ):
            self.assertIn(f'EnvironmentIsOne("{environment}")', PIPELINE_SERIALIZATION)
            self.assertIn(f'std::getenv("{environment}")', PIPELINE_CACHE)
        self.assertIn("fsquerysize={}", PIPELINE_CACHE)
        self.assertIn("fstexelfetch={}", PIPELINE_CACHE)


if __name__ == "__main__":
    unittest.main()
