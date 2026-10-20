import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
BUFFER = (
    ROOT / "funnel-arm" / "src" / "video_core" / "buffer_cache" / "buffer_cache.cpp"
).read_text(encoding="utf-8")


def between(source: str, start: str, end: str) -> str:
    return source.split(start, 1)[1].split(end, 1)[0]


class ReadbackWaveMaterializationContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.wave = between(
            BUFFER,
            "void BufferCache::DownloadBufferMemoryWave(",
            "template <bool async>",
        )

    def test_range_materialization_precedes_dirty_copy_planning(self):
        materialization = self.wave.index("const bool incomplete_range_proof =")
        planning = self.wave.index("const auto queue_buffer_ranges =")
        self.assertLess(materialization, planning)

        prefix = self.wave[materialization:planning]
        self.assertIn(
            "scheduler.FlushPendingBufferTransfersForRange(window_addr, window_size);",
            prefix,
        )
        self.assertIn(
            "for (u32 index = 0; index < recent_readback_window_count; ++index)",
            prefix,
        )
        self.assertIn("const RecentReadbackWindow& window", prefix)
        self.assertIn(
            "scheduler.FlushPendingBufferTransfersForRange(window.begin, window.size);",
            prefix,
        )

    def test_alias_and_replay_keep_the_global_correctness_fallback(self):
        materialization = between(
            self.wave,
            "const bool incomplete_range_proof =",
            "struct WaveCopy",
        )
        self.assertIn("memory->PhysicalAliasingMayExist()", materialization)
        self.assertIn(
            "Libraries::GnmDriver::ExecutorReplayActive()", materialization
        )
        fallback = materialization.index("scheduler.FlushPendingBufferTransfers();")
        scoped = materialization.index(
            "scheduler.FlushPendingBufferTransfersForRange(window_addr, window_size);"
        )
        self.assertLess(fallback, scoped)
        self.assertIn("if (incomplete_range_proof)", materialization)
        self.assertIn("} else {", materialization)

    def test_each_coalesced_wave_copy_gets_an_exact_source_barrier(self):
        barriers = between(
            self.wave,
            "boost::container::small_vector<vk::BufferMemoryBarrier2, 32> pre_barriers;",
            "cmdbuf.pipelineBarrier2(vk::DependencyInfo{",
        )
        self.assertIn("for (const WaveCopy& copy : copies)", barriers)
        self.assertIn(".buffer = copy.buffer->Handle()", barriers)
        self.assertIn(".offset = copy.source_offset", barriers)
        self.assertIn(".size = copy.size", barriers)
        self.assertNotIn("VK_WHOLE_SIZE", barriers)



        for token in (
            "vk::PipelineStageFlagBits2::eAllCommands",
            "vk::AccessFlagBits2::eMemoryWrite",
            "vk::AccessFlagBits2::eShaderWrite",
            "vk::AccessFlagBits2::eTransferWrite",
            "vk::PipelineStageFlagBits2::eTransfer",
            "vk::AccessFlagBits2::eTransferRead",
        ):
            self.assertIn(token, barriers)


if __name__ == "__main__":
    unittest.main()
