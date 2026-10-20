from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
CACHE = (
    ROOT / "funnel-arm/src/video_core/renderer_vulkan/vk_pipeline_cache.cpp"
).read_text(encoding="utf-8")
GRAPHICS = (
    ROOT / "funnel-arm/src/video_core/renderer_vulkan/vk_graphics_pipeline.cpp"
).read_text(encoding="utf-8")
RASTERIZER = (
    ROOT / "funnel-arm/src/video_core/renderer_vulkan/vk_rasterizer.cpp"
).read_text(encoding="utf-8")


class PipelineBuildDiagnosticsGateContractTests(unittest.TestCase):
    def test_async_publication_snapshots_are_profile_only(self):
        for tag in ("[EXECUTOR_ASYNC_SHADER]", "[EXECUTOR_ASYNC_PIPELINE]"):
            log = CACHE.index(tag)
            stats = CACHE.rfind("const auto stats = GetAsyncPipelineCompileStats()", 0, log)
            gate = CACHE.rfind("if (ExecutorGraphicsPipelineLookupProfileEnabled())", 0, stats)
            self.assertNotEqual(gate, -1, tag)
            self.assertLess(log - gate, 2200, tag)

    def test_foreground_warmup_lock_semantics_precede_optional_timing(self):
        start = CACHE.index("PipelineCache::LockBackgroundWarmUpForForeground()")
        end = CACHE.index("void PipelineCache::DiscardBackgroundWarmUpPipelinesLocked", start)
        body = CACHE[start:end]
        self.assertLess(body.index("background_warmup_foreground_waiters.fetch_add"),
                        body.index("const bool collect_wait_profile"))
        self.assertLess(body.index("background_lock = std::unique_lock"),
                        body.index("if (!collect_wait_profile)"))
        self.assertGreater(body.index("background_warmup_wait_events.fetch_add"),
                           body.index("if (!collect_wait_profile)"))

    def test_pipeline_compile_info_logs_are_explicit_opt_in(self):
        self.assertIn("run-diagnostic-pipeline-create-info", GRAPHICS)
        log = GRAPHICS.index("[EXECUTOR_PIPELINE_CI_COUNTS]")
        gate = GRAPHICS.rfind("if (ExecutorPipelineCreateInfoTraceEnabled())", 0, log)
        self.assertNotEqual(gate, -1)
        self.assertLess(log - gate, 800)

    def test_aligned_ssbo_probe_does_not_tax_binding_path(self):
        log = RASTERIZER.index("[EXECUTOR_ALIGNED_SSBO_SCRATCH]")
        gate = RASTERIZER.rfind("if (ExecutorVulkanOptimizationTelemetryEnabled())", 0, log)
        self.assertNotEqual(gate, -1)
        self.assertLess(log - gate, 800)

    def test_periodic_driver_cache_checkpoint_is_off_during_android_gameplay(self):
        start = CACHE.index("void PipelineCache::MaybeCheckpointPipelineCaches()")
        end = CACHE.index("void PipelineCache::ResetPipelineBuildState()", start)
        body = CACHE[start:end]
        gate = body.index("if (!periodic_checkpoint_enabled)")
        clock = body.index("std::chrono::steady_clock::now()")
        save = body.index("SaveHostPipelineCache()")
        self.assertIn("run-enable-periodic-pipeline-cache-checkpoint", body)
        self.assertLess(gate, clock)
        self.assertLess(gate, save)

        destructor = CACHE[CACHE.index("PipelineCache::~PipelineCache()"):
                           CACHE.index("PipelineCache::~PipelineCache()") + 900]
        self.assertIn("SaveHostPipelineCache()", destructor)
        self.assertIn("SavePipelineNegativeCache()", destructor)

    def test_turnip_skips_dead_per_lookup_foreground_clock(self):
        for signature in (
            "const GraphicsPipeline* PipelineCache::GetGraphicsPipeline(",
            "const ComputePipeline* PipelineCache::GetComputePipeline()",
        ):
            start = CACHE.index(signature)
            clock = CACHE.index("AsyncSteadyMilliseconds()", start)
            gate = CACHE.rfind("if (Config::managedAsyncPipelineCompilation()", start, clock)
            block = CACHE[gate:clock]
            self.assertIn("DeferAsyncLiveReplacement(instance)", block)

    def test_first_pipeline_refresh_log_has_no_quiet_path_atomic(self):
        start = CACHE.index(
            "const GraphicsPipeline* PipelineCache::GetGraphicsPipeline("
        )
        end = CACHE.index(
            "const ComputePipeline* PipelineCache::GetComputePipeline()", start
        )
        body = CACHE[start:end]
        counter = body.index("static std::atomic<int> s_gp")
        gate = body.rfind("if (collect_telemetry)", 0, counter)
        self.assertNotEqual(gate, -1)
        self.assertLess(counter - gate, 100)
        self.assertIn("[EXECUTOR_GP]", body[counter:counter + 700])


if __name__ == "__main__":
    unittest.main()
