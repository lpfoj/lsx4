from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
PIPELINE = (
    ROOT / "funnel-arm/src/video_core/renderer_vulkan/vk_pipeline_cache.cpp"
).read_text(encoding="utf-8")
CAPABILITIES = (
    ROOT / "funnel-arm/src/video_core/renderer_vulkan/vk_capability_profile.h"
).read_text(encoding="utf-8")
RUNTIME = (ROOT / "src/executor/native_runtime_api.cpp").read_text(
    encoding="utf-8"
)


class DeferredDriverCheapFirstPipelineContractTests(unittest.TestCase):
    def test_candidate_is_app_private_and_default_off(self):
        self.assertIn(
            '"run-enable-deferred-driver-cheap-first-pipeline"', RUNTIME
        )
        self.assertIn(
            'setenv("EXECUTOR_DEFERRED_DRIVER_CHEAP_FIRST_PIPELINE", "1", 1)',
            RUNTIME,
        )
        self.assertIn(
            'unsetenv("EXECUTOR_DEFERRED_DRIVER_CHEAP_FIRST_PIPELINE")', RUNTIME
        )
        gate = PIPELINE.split("bool UseManagedForegroundFallback", 1)[1].split(
            "vk::PipelineCreateFlags ForegroundFallbackBuildFlags", 1
        )[0]
        self.assertIn(
            'std::getenv("EXECUTOR_DEFERRED_DRIVER_CHEAP_FIRST_PIPELINE")', gate
        )

    def test_non_deferred_behavior_is_unchanged_and_deferred_requires_opt_in(self):
        predicate = PIPELINE.split(
            "constexpr bool CanUseManagedForegroundFallback", 1
        )[1].split("bool UseManagedForegroundFallback", 1)[0]
        for assertion in (
            "static_assert(CanUseManagedForegroundFallback(true, false, false));",
            "static_assert(CanUseManagedForegroundFallback(true, true, true));",
            "static_assert(!CanUseManagedForegroundFallback(true, true, false));",
            "static_assert(!CanUseManagedForegroundFallback(false, false, true));",
        ):
            self.assertIn(assertion, predicate)
        self.assertEqual(2, PIPELINE.count("UseManagedForegroundFallback(instance)"))

    def test_turnip_keeps_no_live_replacement_and_uses_cheap_first_object(self):
        turnip = CAPABILITIES.split(
            "case MobileDriverFamily::MesaTurnip:", 1
        )[1].split("case MobileDriverFamily::Unknown:", 1)[0]
        self.assertIn("CapabilityQuirk::DeferAsyncPipelineReplacement", turnip)
        self.assertNotIn(
            "CapabilityQuirk::ForegroundPipelineWithoutDisableOptimization",
            turnip,
        )
        flags = PIPELINE.split(
            "vk::PipelineCreateFlags ForegroundFallbackBuildFlags", 1
        )[1].split("struct ExecutorGraphicsPipelineLookupProfile", 1)[0]
        self.assertIn("vk::PipelineCreateFlagBits::eDisableOptimization", flags)

    def test_optimized_result_is_cache_only_and_worker_stays_idle_throttled(self):
        worker = PIPELINE.split("void PipelineCache::AsyncPipelineWorker", 1)[1].split(
            "void PipelineCache::PublishAsyncPipelineResults", 1
        )[0]
        self.assertIn("async_last_foreground_activity_ms", worker)
        self.assertIn("+\n                        750u", worker)

        publish = PIPELINE.split(
            "void PipelineCache::PublishAsyncPipelineResults", 1
        )[1].split(
            "std::unique_lock<std::mutex> PipelineCache::LockBackgroundWarmUpForForeground",
            1,
        )[0]
        merge = publish.index("merge_local_cache(result)")
        defer = publish.index(
            "if (optimized && existing_valid &&\n"
            "                            DeferAsyncLiveReplacement(instance))",
            merge,
        )
        early_return = publish.index("return;", defer)
        replacement = publish.index("existing.value() = std::move(result.pipeline)", defer)
        self.assertLess(merge, defer)
        self.assertLess(defer, early_return)
        self.assertLess(early_return, replacement)


if __name__ == "__main__":
    unittest.main()
