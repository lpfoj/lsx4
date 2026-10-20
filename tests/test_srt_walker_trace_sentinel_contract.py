from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
RUNTIME = (ROOT / "src" / "executor" / "native_runtime_api.cpp").read_text(
    encoding="utf-8"
)


class SrtWalkerTraceSentinelContractTests(unittest.TestCase):
    def test_explicit_marker_controls_the_bounded_walker_trace(self) -> None:
        marker = 'std::filesystem::path(g_root) / "run-live-srt-walker-trace"'
        start = RUNTIME.index(marker)
        end = RUNTIME.index("const auto aligned_ssbo_scratch_sentinel", start)
        body = RUNTIME[start:end]

        self.assertIn('setenv("EXECUTOR_TRACE_SRT_WALKER", "1", 1)', body)
        self.assertIn('unsetenv("EXECUTOR_TRACE_SRT_WALKER")', body)
        self.assertIn("[EXECUTOR_SRT_WALKER_TRACE]", body)
        self.assertIn("bounded=1", body)


if __name__ == "__main__":
    unittest.main()
