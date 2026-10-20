import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
RASTERIZER = (
    ROOT / "funnel-arm/src/video_core/renderer_vulkan/vk_rasterizer.cpp"
).read_text(encoding="utf-8")


def between(source: str, start: str, end: str) -> str:
    return source.split(start, 1)[1].split(end, 1)[0]


class RasterizerPipelineBisectCacheContractTests(unittest.TestCase):
    def test_environment_list_is_parsed_once(self):
        parser = between(
            RASTERIZER,
            "static const std::vector<u64>& ExecutorBisectSkippedPipelineHashes() noexcept",
            "static bool ExecutorBisectSkipPipeline(u64 pipeline_hash) noexcept",
        )
        self.assertIn("static const std::vector<u64> hashes = []", parser)
        self.assertEqual(parser.count("std::getenv("), 1)
        self.assertEqual(parser.count("std::strtoull("), 1)
        self.assertIn("std::sort(parsed.begin(), parsed.end())", parser)
        self.assertIn("std::unique(parsed.begin(), parsed.end())", parser)

    def test_per_draw_lookup_has_no_environment_or_number_parsing(self):
        lookup = between(
            RASTERIZER,
            "static bool ExecutorBisectSkipPipeline(u64 pipeline_hash) noexcept",
            "static bool ExecutorNeedsQualcommPostBindDynamicState",
        )
        self.assertIn("ExecutorBisectSkippedPipelineHashes()", lookup)
        self.assertIn("std::binary_search", lookup)
        self.assertNotIn("getenv", lookup)
        self.assertNotIn("strtoull", lookup)


if __name__ == "__main__":
    unittest.main()
