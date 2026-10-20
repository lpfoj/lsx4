import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
BUFFER = (
    ROOT / "funnel-arm" / "src" / "video_core" / "buffer_cache" / "buffer_cache.cpp"
).read_text(encoding="utf-8")


def between(source: str, start: str, end: str) -> str:
    return source.split(start, 1)[1].split(end, 1)[0]


class ReadOnlyRenameLoggingContractTests(unittest.TestCase):
    def test_production_path_never_formats_periodic_rename_diagnostics(self):
        obtain = between(
            BUFFER,
            "std::pair<Buffer*, u32> BufferCache::ObtainReadOnlyBuffer(",
            "std::pair<Buffer*, u32> BufferCache::ObtainBufferForImage(",
        )
        maybe_log = between(obtain, "const auto maybe_log = [&] {", "const auto account_fallback")

        self.assertIn("const bool trace_enabled = read_only_rename_trace_enabled", obtain)
        self.assertIn("if (!trace_enabled)", maybe_log)
        self.assertIn("constexpr u64 log_mask = 16383u", maybe_log)
        self.assertNotIn("Config::managedGpuFastPath()", maybe_log)
        self.assertNotIn("trace_enabled ?", maybe_log)
        self.assertIn("[EXECUTOR_READ_ONLY_RENAME]", maybe_log)

        constructor = between(
            BUFFER,
            "BufferCache::BufferCache(",
            "BufferCache::~BufferCache()",
        )
        self.assertIn(
            "read_only_rename_trace_enabled = ExecutorReadOnlyRenameTraceEnabled()",
            constructor,
        )

    def test_trace_gate_remains_cached_and_explicit(self):
        gate = between(
            BUFFER,
            "bool ExecutorReadOnlyRenameTraceEnabled()",
            "bool ExecutorReadOnlyRenameUsageEnabled(",
        )
        self.assertIn("static const bool enabled", gate)
        self.assertIn("diag-vk-gpu-timeline", gate)
        self.assertIn("std::fopen", gate)
        self.assertNotIn("getenv", gate)


if __name__ == "__main__":
    unittest.main()
