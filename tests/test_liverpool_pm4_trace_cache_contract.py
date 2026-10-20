from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (
    ROOT / "funnel-arm/src/video_core/amdgpu/liverpool.cpp"
).read_text(encoding="utf-8")


class LiverpoolPm4TraceCacheContractTests(unittest.TestCase):
    def test_graphics_parser_reuses_cached_trace_gate(self):
        start = SOURCE.index("Liverpool::Task Liverpool::ProcessGraphics(")
        end = SOURCE.index("Liverpool::Task Liverpool::ProcessCompute(", start)
        body = SOURCE[start:end]
        self.assertIn("const bool kTracePm4 = ExecutorTracePm4();", body)
        self.assertNotIn('std::getenv("EXECUTOR_TRACE_PM4")', body)

    def test_compute_parser_reuses_cached_trace_gate(self):
        start = SOURCE.index("Liverpool::Task Liverpool::ProcessCompute(")
        end = SOURCE.index("void Liverpool::SubmitGfx(", start)
        body = SOURCE[start:end]
        self.assertIn("const bool kTracePm4 = ExecutorTracePm4();", body)
        self.assertNotIn('std::getenv("EXECUTOR_TRACE_PM4")', body)


if __name__ == "__main__":
    unittest.main()
