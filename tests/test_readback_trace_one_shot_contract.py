from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "src/executor/native_runtime_api.cpp").read_text(encoding="utf-8")


class ReadbackTraceOneShotContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        start = SOURCE.index('unsetenv("EXECUTOR_READBACK_CONTRACT_TRACE")')
        end = SOURCE.index("const auto shader_identity_cache_sentinel", start)
        cls.body = SOURCE[start:end]

    def test_every_runtime_start_clears_inherited_trace_state(self):
        self.assertTrue(
            self.body.startswith('unsetenv("EXECUTOR_READBACK_CONTRACT_TRACE")')
        )

    def test_legacy_persistent_marker_is_cleanup_only(self):
        legacy_start = self.body.index('"run-readback-contract-trace"')
        one_shot_start = self.body.index('"run-readback-contract-trace-once"')
        legacy = self.body[legacy_start:one_shot_start]
        self.assertIn("std::filesystem::remove(", legacy)
        self.assertIn("source=legacy-cleanup", legacy)
        self.assertNotIn('setenv("EXECUTOR_READBACK_CONTRACT_TRACE"', legacy)

    def test_one_shot_marker_is_consumed_before_trace_is_enabled(self):
        one_shot_start = self.body.index('"run-readback-contract-trace-once"')
        one_shot = self.body[one_shot_start:]
        remove = one_shot.index("std::filesystem::remove(")
        success_gate = one_shot.index("if (readback_trace_once_consumed)")
        enable = one_shot.index('setenv("EXECUTOR_READBACK_CONTRACT_TRACE"')
        self.assertLess(remove, success_gate)
        self.assertLess(success_gate, enable)
        self.assertIn("source=one-shot consumed=1", one_shot)

    def test_failed_one_shot_cleanup_cannot_enable_trace(self):
        one_shot_start = self.body.index('"run-readback-contract-trace-once"')
        one_shot = self.body[one_shot_start:]
        failure_gate = one_shot.index("else if (readback_trace_once_error)")
        failure = one_shot[failure_gate:]
        self.assertIn("consumed=0 enabled=0", failure)
        self.assertNotIn('setenv("EXECUTOR_READBACK_CONTRACT_TRACE"', failure)


if __name__ == "__main__":
    unittest.main()
