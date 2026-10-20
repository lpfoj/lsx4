import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
RASTERIZER = (
    ROOT / "funnel-arm/src/video_core/renderer_vulkan/vk_rasterizer.cpp"
).read_text(encoding="utf-8")


def source_between(start: str, end: str) -> str:
    return RASTERIZER.split(start, 1)[1].split(end, 1)[0]


class DrawStageProfileContractTests(unittest.TestCase):
    def test_profile_is_cached_marker_only_and_default_off(self):
        gate = source_between(
            "bool ExecutorDrawStageProfileEnabled()",
            "struct ExecutorDrawCpuClockSample",
        )
        self.assertIn("static const bool enabled", gate)
        self.assertIn("run-diagnostic-draw-stage-profile", gate)
        self.assertIn("::access", gate)
        self.assertNotIn("getenv", gate)

    def test_wall_and_thread_cpu_clocks_are_behind_enabled_guard(self):
        clock = source_between(
            "ExecutorDrawCpuClockSample ExecutorDrawCpuNow()",
            "u64 ExecutorDrawCpuDelta",
        )
        self.assertIn("std::chrono::steady_clock::now()", clock)
        self.assertIn("clock_gettime(CLOCK_THREAD_CPUTIME_ID", clock)

        constructor = source_between(
            "explicit ExecutorDrawStageProfile", "~ExecutorDrawStageProfile()"
        )
        guard = constructor.index("if (!enabled_)")
        present = constructor.index("ExecutorGetLivePresentCount()")
        clock_call = constructor.index("ExecutorDrawCpuNow()")
        self.assertLess(guard, present)
        self.assertLess(guard, clock_call)

    def test_publish_rate_is_below_five_hz_and_windows_reset(self):
        self.assertIn(
            "ExecutorDrawProfilePublishIntervalNs = 1'000'000'000ull",
            RASTERIZER,
        )
        commit = source_between(
            "void Commit(const ExecutorDrawCpuClockSample& draw_end)",
            "static void Publish",
        )
        self.assertIn("ExecutorDrawProfilePublishIntervalNs", commit)
        publisher = source_between(
            "static void Publish", "bool enabled_{}"
        )
        self.assertEqual(RASTERIZER.count("[EXECUTOR_DRAW_STAGE_PROFILE]"), 1)
        self.assertIn("draw-stage-profile.txt", publisher)
        self.assertIn("totals = {}", publisher)
        self.assertIn("totals.window_begin_wall_ns = publish_wall_ns", publisher)

    def test_publication_is_three_bounded_correlated_lines(self):
        publisher = source_between("static void Publish", "bool enabled_{}")
        self.assertIn("ExecutorDrawProfileLineCapacity = 1800", publisher)
        self.assertNotIn("3584", publisher)
        for line in ("summary_line", "stages_line", "obtain_line"):
            self.assertIn(
                f"std::array<char, ExecutorDrawProfileLineCapacity> {line}",
                publisher,
            )
        for part in ("summary", "stages", "obtain"):
            self.assertIn(f"part={part} parts=3", publisher)
        self.assertEqual(publisher.count("schema=1 window=%llu part="), 3)
        self.assertIn("const std::array<const char*, 3> profile_lines", publisher)
        self.assertEqual(
            publisher.count("for (const char* profile_line : profile_lines)"), 2
        )
        self.assertIn("std::fputs(profile_line, out)", publisher)
        self.assertIn(
            '"[EXECUTOR_DRAW_STAGE_PROFILE] %s"', publisher
        )

        stages = publisher[
            publisher.index("append(stages_line") : publisher.index(
                "append(obtain_line"
            )
        ]
        self.assertIn("StageNames", stages)
        self.assertIn("vertexIndex", stages)
        self.assertIn("beginRendering", stages)
        self.assertIn("drawEmit", stages)
        obtain = publisher[publisher.index("append(obtain_line") :]
        self.assertIn("obtainCalls", obtain)
        self.assertIn("obtainMaxWallMs", obtain)

    def test_all_requested_draw_stages_report_wall_cpu_and_off_cpu(self):
        for token in (
            "PipelineLookup",
            "PrepareRenderState",
            "BindBuffers",
            "BindTextures",
            "BindSamplers",
            "VertexIndex",
            "BeginRendering",
            "DrawEmit",
            '"pipeline"',
            '"prepare"',
            '"buffers"',
            '"textures"',
            '"samplers"',
            '"vertexIndex"',
            '"beginRendering"',
            '"drawEmit"',
            "WallMsPerFrame",
            "CpuMsPerFrame",
            "OffCpuMsPerFrame",
        ):
            self.assertIn(token, RASTERIZER)

    def test_obtain_buffer_is_measured_without_buffer_cache_hooks(self):
        bind_buffers = source_between(
            "void Rasterizer::BindBuffers", "void Rasterizer::BindTextures"
        )
        self.assertIn("ExecutorDrawProfileObtain([&]", bind_buffers)
        self.assertIn("buffer_cache.ObtainReadOnlyBuffer", bind_buffers)
        self.assertIn("buffer_cache.ObtainBuffer", bind_buffers)
        self.assertIn("obtainFast", RASTERIZER)
        self.assertIn("obtainSlow", RASTERIZER)
        self.assertIn("obtainOffCpuMsPerFrame", RASTERIZER)
        self.assertIn("obtainSlowOffCpuMsPerFrame", RASTERIZER)
        self.assertIn("otherOffCpuMsPerFrame", RASTERIZER)

        buffer_cache = (
            ROOT / "funnel-arm/src/video_core/buffer_cache/buffer_cache.cpp"
        ).read_text(encoding="utf-8")
        self.assertNotIn("EXECUTOR_DRAW_STAGE_PROFILE", buffer_cache)

    def test_draw_calls_are_only_observed_and_never_changed(self):
        draw = source_between(
            "void Rasterizer::Draw(bool is_indexed", "void Rasterizer::DrawIndirect"
        )
        self.assertEqual(draw.count("cmdbuf.drawIndexed("), 1)
        self.assertEqual(draw.count("cmdbuf.draw("), 1)
        emit_begin = draw.index(
            "draw_stage_profile.Begin(ExecutorDrawCpuStage::DrawEmit)"
        )
        indexed = draw.index("cmdbuf.drawIndexed(")
        emit_end = draw.index(
            "draw_stage_profile.End(ExecutorDrawCpuStage::DrawEmit)"
        )
        self.assertLess(emit_begin, indexed)
        self.assertLess(indexed, emit_end)
        self.assertNotIn("sleep", draw)


if __name__ == "__main__":
    unittest.main()
