from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (
    ROOT / "funnel-arm/src/video_core/renderer_vulkan/vk_pipeline_cache.cpp"
).read_text(encoding="utf-8")
HEADER = (
    ROOT / "funnel-arm/src/video_core/renderer_vulkan/vk_pipeline_cache.h"
).read_text(encoding="utf-8")


def source_between(start: str, end: str) -> str:
    return SOURCE.split(start, 1)[1].split(end, 1)[0]


class AsyncPipelineResultReadyContractTests(unittest.TestCase):
    def test_each_result_queue_has_an_independent_atomic_ready_flag(self):
        self.assertIn(
            "std::atomic_bool async_pipeline_results_ready{false};", HEADER
        )
        self.assertIn(
            "std::atomic_bool async_shader_results_ready{false};", HEADER
        )

    def test_shader_producer_publishes_flag_after_queue_push(self):
        worker = source_between(
            "void PipelineCache::AsyncShaderWorker(",
            "void PipelineCache::PublishAsyncShaderResults()",
        )
        lock = worker.rindex("std::scoped_lock lock{async_pipeline_mutex}")
        push = worker.index("async_shader_results.push_back", lock)
        publish = worker.index(
            "async_shader_results_ready.store(true, std::memory_order_release)",
            push,
        )
        self.assertLess(lock, push)
        self.assertLess(push, publish)

    def test_pipeline_producer_publishes_flag_after_queue_push(self):
        worker = source_between(
            "void PipelineCache::AsyncPipelineWorker(",
            "void PipelineCache::PublishAsyncPipelineResults()",
        )
        lock = worker.rindex("std::scoped_lock lock{async_pipeline_mutex}")
        push = worker.index("async_pipeline_results.push_back", lock)
        publish = worker.index(
            "async_pipeline_results_ready.store(true, std::memory_order_release)",
            push,
        )
        self.assertLess(lock, push)
        self.assertLess(push, publish)

    def test_shader_consumer_checks_acquire_before_mutex_and_clears_under_lock(self):
        consumer = source_between(
            "void PipelineCache::PublishAsyncShaderResults()",
            "void PipelineCache::DestroyRetiredShaderModules()",
        )
        acquire = consumer.index(
            "async_shader_results_ready.load(std::memory_order_acquire)"
        )
        lock = consumer.index("std::scoped_lock lock{async_pipeline_mutex}")
        swap = consumer.index("completed.swap(async_shader_results)", lock)
        clear = consumer.index(
            "async_shader_results_ready.store(false, std::memory_order_release)",
            swap,
        )
        self.assertLess(acquire, lock)
        self.assertLess(lock, swap)
        self.assertLess(swap, clear)

    def test_pipeline_consumer_checks_acquire_before_mutex_and_preserves_shader_drain(self):
        consumer = source_between(
            "void PipelineCache::PublishAsyncPipelineResults()",
            "std::unique_lock<std::mutex> PipelineCache::LockBackgroundWarmUpForForeground()",
        )
        acquire = consumer.index(
            "async_pipeline_results_ready.load(std::memory_order_acquire)"
        )
        fast_shader_drain = consumer.index("PublishAsyncShaderResults()", acquire)
        fast_return = consumer.index("return;", fast_shader_drain)
        lock = consumer.index("std::scoped_lock lock{async_pipeline_mutex}")
        swap = consumer.index("completed.swap(async_pipeline_results)", lock)
        clear = consumer.index(
            "async_pipeline_results_ready.store(false, std::memory_order_release)",
            swap,
        )
        self.assertLess(acquire, fast_shader_drain)
        self.assertLess(fast_shader_drain, fast_return)
        self.assertLess(fast_return, lock)
        self.assertLess(lock, swap)
        self.assertLess(swap, clear)
        self.assertGreaterEqual(consumer.count("PublishAsyncShaderResults();"), 2)
        self.assertGreaterEqual(consumer.count("DestroyRetiredShaderModules();"), 2)


if __name__ == "__main__":
    unittest.main()
