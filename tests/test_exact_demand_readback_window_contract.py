import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
BUFFER = (
    ROOT / "funnel-arm" / "src" / "video_core" / "buffer_cache" / "buffer_cache.cpp"
).read_text(encoding="utf-8")


def between(source: str, start: str, end: str) -> str:
    return source.split(start, 1)[1].split(end, 1)[0]


class ExactDemandReadbackWindowContractTests(unittest.TestCase):
    def test_gate_is_cached_marker_only_and_has_startup_proof(self):
        gate = between(
            BUFFER,
            "bool ExecutorExactDemandReadbackWindowEnabled()",
            "bool ExecutorSnapshotDependencyResolutionEnabled()",
        )
        self.assertIn("run-enable-exact-demand-readback-window", gate)
        self.assertIn("static const bool enabled", gate)
        self.assertIn("std::fopen", gate)
        self.assertNotIn("getenv", gate)

        constructor = between(
            BUFFER,
            "BufferCache::BufferCache(",
            "BufferCache::~BufferCache()",
        )
        self.assertIn("ExecutorExactDemandReadbackWindowEnabled()", constructor)
        self.assertIn("[EXECUTOR_EXACT_DEMAND_READBACK_CONFIG]", constructor)
        self.assertIn("requestWindow=exact", constructor)

    def test_enabled_gate_keeps_the_mandatory_window_exact(self):
        read = between(
            BUFFER,
            "void BufferCache::ReadMemory(",
            "void BufferCache::ResolvePendingReadback(",
        )
        gate = read.index("const bool exact_demand_window =")
        find_buffer = read.index("FindBuffer(device_addr, size)")
        self.assertLess(gate, find_buffer)



        validity = read[gate:find_buffer]
        for token in (
            "size != 0",
            "size <= std::numeric_limits<u32>::max()",
            "device_addr <= std::numeric_limits<VAddr>::max() - size",
        ):
            self.assertIn(token, validity)
        self.assertNotIn("return;", validity)




        self.assertIn("VAddr download_addr = device_addr;", read)
        self.assertIn("u64 download_size = size;", read)
        expansion = read.index(
            "if (!exact_demand_window && managed_submit_readback &&"
        )
        wave = read.index("DownloadBufferMemoryWave(buffer, download_addr, download_size")
        self.assertLess(expansion, wave)
        expansion_body = read[expansion:wave]
        self.assertIn("Common::AlignDown(device_addr, ExecutorPreciseReadbackWindow)", expansion_body)
        self.assertIn("window_begin + ExecutorPreciseReadbackWindow", expansion_body)
        for unchanged_condition in (
            "size <= TRACKER_BYTES_PER_PAGE",
            "guest_memory != nullptr",
            "!guest_memory->PhysicalAliasingMayExist()",
        ):
            self.assertIn(unchanged_condition, expansion_body)
        self.assertEqual(read.count("exact_demand_window"), 2)

    def test_submit_prefetch_and_recent_wave_logic_do_not_consult_gate(self):
        prefetch = between(
            BUFFER,
            "void BufferCache::PrefetchReadbackWindows()",
            "void BufferCache::DownloadBufferMemoryWave(",
        )
        wave = between(
            BUFFER,
            "void BufferCache::DownloadBufferMemoryWave(",
            "template <bool async>",
        )
        self.assertNotIn("ExecutorExactDemandReadbackWindowEnabled", prefetch)
        self.assertNotIn("exact_demand_window", prefetch)
        self.assertNotIn("ExecutorExactDemandReadbackWindowEnabled", wave)
        self.assertNotIn("exact_demand_window", wave)
        self.assertIn("recent_readback_window_count", wave)
        self.assertIn("queue_window(recent_readback_windows[index])", wave)


if __name__ == "__main__":
    unittest.main()
