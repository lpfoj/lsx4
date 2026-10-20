import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = (
    ROOT
    / "funnel-arm"
    / "src"
    / "video_core"
    / "renderer_vulkan"
    / "vk_graphics_pipeline.cpp"
).read_text(encoding="utf-8")


class ExpectedPipelineCompileRequiredLoggingContract(unittest.TestCase):
    def test_expected_fail_fast_result_skips_android_error_log(self):
        failure = SOURCE.index("if (pipeline_result != vk::Result::eSuccess)")
        success = SOURCE.index("pipeline = std::move(pipe)", failure)
        block = SOURCE[failure:success]
        expected_gate = block.index(
            "if (pipeline_result != vk::Result::ePipelineCompileRequired)"
        )
        android_log = block.index("__android_log_print", expected_gate)
        generic_log = block.index("LOG_ERROR", expected_gate)
        self.assertLess(expected_gate, android_log)
        self.assertLess(expected_gate, generic_log)

    def test_unexpected_pipeline_failures_keep_full_diagnostics(self):
        self.assertIn("[EXECUTOR_GRAPHICS_PIPELINE_FAILURE] phase=pipeline", SOURCE)
        self.assertIn("Failed to create graphics pipeline", SOURCE)


if __name__ == "__main__":
    unittest.main()
