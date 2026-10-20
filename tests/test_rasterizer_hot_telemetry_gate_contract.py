import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
RASTERIZER = (
    ROOT / "funnel-arm/src/video_core/renderer_vulkan/vk_rasterizer.cpp"
).read_text(encoding="utf-8")


def between(source: str, start: str, end: str) -> str:
    return source.split(start, 1)[1].split(end, 1)[0]


class RasterizerHotTelemetryGateContractTests(unittest.TestCase):
    def test_cached_markers_are_explicit_and_default_off(self):
        invalidation_gate = between(
            RASTERIZER,
            "bool ExecutorGuestInvalidationProfileEnabled() noexcept",
            "bool ExecutorVulkanOptimizationTelemetryEnabled() noexcept",
        )
        self.assertIn("static const bool enabled", invalidation_gate)
        self.assertIn(
            "run-diagnostic-guest-invalidation-profile", invalidation_gate
        )
        self.assertIn("::access", invalidation_gate)
        self.assertNotIn("getenv", invalidation_gate)

        optimization_gate = between(
            RASTERIZER,
            "bool ExecutorVulkanOptimizationTelemetryEnabled() noexcept",
            "bool ExecutorBoundedGuestInvalidationDrainEnabled() noexcept",
        )
        self.assertIn("static const bool enabled", optimization_gate)
        self.assertIn(
            "run-diagnostic-vulkan-optimization-stats", optimization_gate
        )
        self.assertIn("::access", optimization_gate)
        self.assertNotIn("getenv", optimization_gate)

        constructor = between(
            RASTERIZER,
            "Rasterizer::Rasterizer(const Instance& instance_",
            "Rasterizer::~Rasterizer()",
        )
        proof_initializer = between(
            RASTERIZER,
            "void ExecutorInitializeRuntimeProofCounters() noexcept",
            "bool ExecutorVulkanOptimizationTelemetryEnabled() noexcept",
        )
        self.assertIn(
            "const bool enabled = ExecutorGuestInvalidationProfileEnabled()",
            proof_initializer,
        )
        self.assertLess(
            proof_initializer.index("if (!enabled)"),
            proof_initializer.index(
                "executor_guest_invalidation_requests.store("
            ),
        )
        self.assertLess(
            constructor.index("ExecutorInitializeRuntimeProofCounters()"),
            constructor.index("memory->SetRasterizer("),
        )
        self.assertLess(
            constructor.index("ExecutorVulkanOptimizationTelemetryEnabled()"),
            constructor.index("memory->SetRasterizer("),
        )

    def test_pipeline_bind_decision_precedes_optional_duplicate_telemetry(self):
        methods = (
            between(
                RASTERIZER,
                "void Rasterizer::ExecutorBindGuestGraphicsPipeline",
                "void Rasterizer::ExecutorSealGuestGraphicsPipelineState",
            ),
            between(
                RASTERIZER,
                "void Rasterizer::ExecutorBindGuestComputePipeline",
                "struct ExecutorDepthWriteOracleRuntime",
            ),
        )
        for method in methods:
            with self.subTest(method=method[:60]):
                bind = method.index("if (needs_bind)")
                record = method.index("VideoCore::RecordVulkanPipelineBind")
                telemetry = method.index(
                    "if (ExecutorVulkanOptimizationTelemetryEnabled())"
                )
                self.assertLess(bind, record)
                self.assertLess(record, telemetry)
                telemetry_body = method[telemetry:]
                self.assertIn("static thread_local u64 attempts", telemetry_body)
                self.assertIn("static thread_local u64 hits", telemetry_body)
                self.assertIn("++attempts", telemetry_body)
                self.assertIn("[EXECUTOR_VK_BIND_DEDUP]", telemetry_body)

    def test_invalidation_clocks_and_observability_atomics_are_opt_in(self):
        invalidate = between(
            RASTERIZER,
            "bool Rasterizer::InvalidateMemoryFromGuestThread(\n    VAddr addr",
            "bool Rasterizer::InvalidateMemoryBatchOnGpuThread",
        )
        self.assertIn(
            "executor_guest_invalidation_profile_enabled =\n"
            "        ExecutorGuestInvalidationProfileEnabled()",
            invalidate,
        )
        self.assertIn(
            "executor_guest_invalidation_overlap_enabled =\n"
            "        VideoCore::InvalidationOverlapTrace::Enabled()",
            invalidate,
        )
        self.assertIn(
            "executor_guest_invalidation_timing_enabled\n"
            "                                ? std::chrono::steady_clock::now()",
            invalidate,
        )
        self.assertIn(
            "if (executor_guest_invalidation_timing_enabled)", invalidate
        )
        self.assertIn(
            "if (executor_guest_invalidation_overlap_enabled)", invalidate
        )
        overlap_capture = invalidate.index(
            "VideoCore::InvalidationOverlapTrace::BeginRequestV2("
        )
        overlap_gate = invalidate.rfind(
            "executor_guest_invalidation_overlap_enabled\n"
            "            ?",
            0,
            overlap_capture,
        )
        self.assertGreaterEqual(overlap_gate, 0)
        self.assertIn(
            "if (executor_guest_invalidation_profile_enabled)", invalidate
        )
        self.assertLess(
            invalidate.index("pending_guest_invalidations.push_back(&request)"),
            invalidate.index("executor_guest_invalidation_queue_max"),
        )
        counter_add = between(
            RASTERIZER,
            "u64 ExecutorRuntimeProofCounterAdd(",
            "void ExecutorInitializeRuntimeProofCounters() noexcept",
        )
        self.assertIn(
            "counter.fetch_add(delta, std::memory_order_relaxed)",
            counter_add,
        )
        self.assertIn(
            "executor_runtime_proof_invalid_mask.fetch_or(", counter_add
        )
        for counter in (
            "executor_guest_invalidation_requests",
            "executor_guest_invalidation_retries",
            "executor_guest_invalidation_completions",
            "executor_guest_invalidation_wait_ns",
        ):
            update = re.search(
                rf"(?:{counter}\.fetch_add|"
                rf"ExecutorRuntimeProofCounterAdd\(\s*{counter}\s*,)",
                invalidate,
            )
            self.assertIsNotNone(update, counter)
            position = update.start()
            gate = invalidate.rfind(
                "if (executor_guest_invalidation_profile_enabled)", 0, position
            )
            self.assertGreaterEqual(gate, 0, counter)

    def test_drain_profile_counters_do_not_control_completion(self):
        drain = between(
            RASTERIZER,
            "void Rasterizer::DrainGuestInvalidationsImpl()",
            "bool Rasterizer::ReadMemory(",
        )
        self.assertIn(
            "const bool executor_guest_invalidation_profile_enabled",
            drain,
        )
        for counter in (
            "executor_guest_invalidation_batches.fetch_add",
            "executor_guest_invalidation_catchup_requests.fetch_add",
            "executor_guest_invalidation_drain_rounds.fetch_add",
        ):
            position = drain.index(counter)
            gate = drain.rfind(
                "if (executor_guest_invalidation_profile_enabled)", 0, position
            )
            self.assertGreaterEqual(gate, 0, counter)
        self.assertIn("process_batch(batch)", drain)
        self.assertIn("request->complete = true", drain)
        self.assertIn("guest_invalidation_completion_cv.notify_one()", drain)


if __name__ == "__main__":
    unittest.main()
