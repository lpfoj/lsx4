import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
VIDEO = ROOT / "funnel-arm" / "src" / "video_core"
READBACK_H = (VIDEO / "readback_optimization.h").read_text(encoding="utf-8")
READBACK_CPP = (VIDEO / "readback_optimization.cpp").read_text(encoding="utf-8")
BUFFER = (VIDEO / "buffer_cache" / "buffer_cache.cpp").read_text(encoding="utf-8")
TEXTURE_H = (VIDEO / "texture_cache" / "texture_cache.h").read_text(encoding="utf-8")
TEXTURE = (VIDEO / "texture_cache" / "texture_cache.cpp").read_text(encoding="utf-8")
RASTERIZER = (VIDEO / "renderer_vulkan" / "vk_rasterizer.cpp").read_text(
    encoding="utf-8"
)
PAGE_MANAGER = (VIDEO / "page_manager.cpp").read_text(encoding="utf-8")


def between(source: str, start: str, end: str) -> str:
    return source.split(start, 1)[1].split(end, 1)[0]


class ReadbackClassificationContractTests(unittest.TestCase):
    def test_gate_is_exact_cached_marker_and_not_environment_driven(self):
        gate = between(
            READBACK_CPP,
            "bool IsReadbackClassificationEnabled() noexcept",
            "void RecordBufferDemandWaveClassification",
        )
        self.assertIn("run-diagnostic-readback-classify", gate)
        self.assertIn("static const bool enabled", gate)
        self.assertIn("::access", gate)
        self.assertNotIn("getenv", gate)

    def test_buffer_wave_reports_exact_request_actual_download_wait_and_total(self):
        wave = between(
            BUFFER,
            "void BufferCache::DownloadBufferMemoryWave",
            "template <bool async>",
        )
        wait = wave.index("scheduler.WaitRetirement(completion_tick)")
        record = wave.index("RecordBufferDemandWaveClassification")
        self.assertLess(wait, record)
        record_body = wave[record : record + 260]
        for token in (
            "access_size",
            "copied_bytes",
            "wait_ns",
            "ExecutorElapsedNs(plan_start)",
        ):
            self.assertIn(token, record_body)
        self.assertIn("if (IsReadbackClassificationEnabled())", wave)

        split = wave.index("DownloadBufferMemoryWave(primary_buffer, chunk_begin")
        split_body = wave[split : split + 260]
        self.assertIn("exact_size", split_body)
        self.assertIn("disjoint portions", wave)

    def test_texture_gc_and_linear_downloads_are_separate_and_map_is_bracketed(self):
        self.assertIn("enum class ImageDownloadClassification", TEXTURE_H)
        process = between(
            TEXTURE,
            "void TextureCache::ProcessDownloadImages()",
            "bool TextureCache::DownloadImageMemory",
        )
        self.assertIn("ImageDownloadClassification::LinearImage", process)

        download = between(
            TEXTURE,
            "bool TextureCache::DownloadImageMemory",
            "#if defined(__ANDROID__)",
        )
        gate = download.index("IsReadbackClassificationEnabled()")
        total_clock = download.index("std::chrono::steady_clock::now()", gate)
        map_start = download.index("classify_map_start")
        mapping = download.index("download_buffer.Map(download_size)")
        map_elapsed = download.index("classify_map_wait_ns =", mapping)
        self.assertLess(gate, total_clock)
        self.assertLess(map_start, mapping)
        self.assertLess(mapping, map_elapsed)
        self.assertIn("SCOPE_EXIT", download)
        self.assertIn("RecordTextureReadbackClassification", download)

        gc = between(
            TEXTURE,
            "void TextureCache::GarbageCollectImages",
            "void TextureCache::GarbageCollectSamplers",
        )
        self.assertIn("ImageDownloadClassification::GarbageCollection", gc)

    def test_critical_texture_finish_and_clean_buffer_evictions_are_counted(self):
        texture_gc = between(
            TEXTURE,
            "void TextureCache::GarbageCollectImages",
            "void TextureCache::GarbageCollectSamplers",
        )
        finish = texture_gc.index("scheduler.Finish(")
        finish_record = texture_gc.index(
            "RecordTextureGcCriticalFinishClassification"
        )
        self.assertLess(finish, finish_record)
        self.assertIn("if (classify_finish)", texture_gc)

        buffer_gc = between(
            BUFFER,
        "void BufferCache::RunGarbageCollector(",
            "void BufferCache::TouchBuffer",
        )
        deletion = buffer_gc.index("DeleteBuffer(buffer_id)")
        eviction_record = buffer_gc.index(
            "RecordBufferCleanEvictionsClassification(clean_evicted)"
        )
        self.assertLess(deletion, eviction_record)
        self.assertIn("clean_evicted != 0", buffer_gc)

    def test_publisher_is_rate_limited_and_only_called_from_submit_context(self):
        publisher = between(
            READBACK_CPP,
            "void MaybeLogReadbackClassification() noexcept",
            "#undef LSX4_READBACK_STATS_FIELDS",
        )
        self.assertIn("constexpr u64 LogIntervalNs = 200'000'000", publisher)
        self.assertIn("std::scoped_lock lock", publisher)
        self.assertEqual(READBACK_CPP.count("[EXECUTOR_READBACK_CLASSIFY]"), 1)

        submit = between(
            RASTERIZER,
            "void Rasterizer::OnSubmit()",
            "void Rasterizer::DisableFexDeferredWriteEpochsForPhysicalAliasing",
        )
        self.assertIn("VideoCore::MaybeLogReadbackClassification()", submit)
        self.assertLess(
            submit.index("buffer_cache.RunGarbageCollector(cache_pressure_sample)"),
            submit.index("VideoCore::MaybeLogReadbackClassification()"),
        )
        self.assertNotIn("MaybeLogReadbackClassification", PAGE_MANAGER)

    def test_log_exposes_window_sums_amplification_and_cumulative_maxima(self):
        for token in (
            "dBufReqBytes",
            "dBufDownloadedBytes",
            "dBufAmplification",
            "dBufWaitMs",
            "dBufTotalMs",
            "bufMaxWaitMs",
            "bufMaxTotalMs",
            "dTexGcBytes",
            "dTexGcMapWaitMs",
            "dTexGcTotalMs",
            "texGcMaxMapWaitMs",
            "texGcMaxTotalMs",
            "dGcCriticalFinishWaitMs",
            "dTexLinearBytes",
            "dTexLinearMapWaitMs",
            "dTexLinearTotalMs",
            "texLinearMaxMapWaitMs",
            "texLinearMaxTotalMs",
            "dBufCleanEvict",
        ):
            self.assertIn(token, READBACK_CPP)
        for token in (
            "buffer_demand_max_wait_ns",
            "buffer_demand_max_total_ns",
            "texture_gc_max_map_wait_ns",
            "texture_gc_max_total_ns",
            "texture_linear_max_map_wait_ns",
            "texture_linear_max_total_ns",
        ):
            self.assertIn(token, READBACK_H)


if __name__ == "__main__":
    unittest.main()
