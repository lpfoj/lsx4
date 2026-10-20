import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
VK = ROOT / "funnel-arm" / "src" / "video_core" / "renderer_vulkan"
COMPUTE = (VK / "vk_compute_pipeline.cpp").read_text(encoding="utf-8")
CAPABILITY = (VK / "vk_capability_profile.h").read_text(encoding="utf-8")


def source_between(source: str, start: str, end: str) -> str:
    return source.split(start, 1)[1].split(end, 1)[0]


class ComputePipelineCreateDiagnosticContractTests(unittest.TestCase):
    def test_marker_gate_is_android_only_cached_and_explicit(self):
        gate = source_between(
            COMPUTE,
            "bool ComputePipelineCreateDiagnosticRequested() noexcept",
            "} // namespace",
        )
        self.assertIn("static const bool requested", gate)
        self.assertIn("diag-compute-pipeline-create", gate)
        self.assertIn("::access", gate)
        self.assertIn("F_OK", gate)
        self.assertNotIn("getenv", gate)

        definition = COMPUTE.index(
            "bool ComputePipelineCreateDiagnosticRequested() noexcept"
        )
        android_begin = COMPUTE.rfind("#ifdef __ANDROID__", 0, definition)
        android_end = COMPUTE.index("#endif", definition)
        self.assertLess(android_begin, definition)
        self.assertLess(definition, android_end)

    def test_begin_and_end_records_bracket_the_single_driver_call(self):
        constructor = source_between(
            COMPUTE,
            "ComputePipeline::ComputePipeline(",
            "ComputePipeline::~ComputePipeline()",
        )
        call = "createComputePipelineUnique(pipeline_cache, compute_pipeline_ci)"
        self.assertEqual(constructor.count(call), 1)
        call_index = constructor.index(call)

        begin_index = constructor.index(
            "[EXECUTOR_COMPUTE_PIPELINE_CREATE] event=begin"
        )
        end_index = constructor.index(
            "[EXECUTOR_COMPUTE_PIPELINE_CREATE] event=end"
        )
        self.assertLess(begin_index, call_index)
        self.assertLess(call_index, end_index)

        self.assertEqual(constructor.count("if (diagnose_pipeline_create)"), 2)
        self.assertEqual(constructor.count("diagnose_pipeline_create"), 3)
        self.assertLess(
            constructor.index("build_result = pipeline_result"),
            constructor.index("if (pipeline_result != vk::Result::eSuccess)"),
        )
        self.assertLess(end_index, constructor.index("build_result = pipeline_result"))

    def test_records_contain_the_compact_reproduction_fields(self):
        constructor = source_between(
            COMPUTE,
            "ComputePipeline::ComputePipeline(",
            "ComputePipeline::~ComputePipeline()",
        )
        begin = source_between(
            constructor,
            "[EXECUTOR_COMPUTE_PIPELINE_CREATE] event=begin",
            "#endif",
        )
        end = source_between(
            constructor,
            "[EXECUTOR_COMPUTE_PIPELINE_CREATE] event=end",
            "#endif",
        )

        for field in (
            "key=",
            "pgmHash=",
            "pgmBase=",
            "workgroup=",
            "subgroupOps=",
            "requireSubgroup64=",
            "buffers=",
            "images=",
            "samplers=",
            "storageImages=",
            "preloading=",
            "flags=",
        ):
            self.assertIn(field, begin)
        for field in ("key=", "pgmHash=", "result="):
            self.assertIn(field, end)

    def test_logging_is_explicit_on_every_driver_and_observational(self):
        constructor = source_between(
            COMPUTE,
            "ComputePipeline::ComputePipeline(",
            "ComputePipeline::~ComputePipeline()",
        )
        diagnostic_gate = source_between(
            constructor,
            "const bool diagnose_pipeline_create =",
            ";\n    if (diagnose_pipeline_create)",
        )
        self.assertIn("ComputePipelineCreateDiagnosticRequested()", diagnostic_gate)
        self.assertNotIn("HasQuirk", diagnostic_gate)
        self.assertNotIn("CapabilityQuirk", diagnostic_gate)

        first_log = constructor.index("if (diagnose_pipeline_create)")
        driver_call = constructor.index("createComputePipelineUnique")
        first_log_end = constructor.index("#endif", first_log)
        second_log = constructor.index("if (diagnose_pipeline_create)", driver_call)
        second_log_end = constructor.index("#endif", second_log)
        self.assertLess(first_log_end, driver_call)
        self.assertLess(driver_call, second_log)
        for block in (
            constructor[first_log:first_log_end],
            constructor[second_log:second_log_end],
        ):
            self.assertNotIn("return", block)
            self.assertNotIn("build_result =", block)
            self.assertNotIn("pipeline =", block)
            self.assertNotIn("Wait(", block)
            self.assertNotIn("Finish(", block)


if __name__ == "__main__":
    unittest.main()
