import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SCHEDULER = (
    ROOT / "funnel-arm/src/video_core/renderer_vulkan/vk_scheduler.cpp"
).read_text(encoding="utf-8")
HEADER = (
    ROOT / "funnel-arm/src/video_core/renderer_vulkan/vk_scheduler.h"
).read_text(encoding="utf-8")


def between(source: str, start: str, end: str) -> str:
    return source.split(start, 1)[1].split(end, 1)[0]


def braced_block(source: str, marker: str, start: int = 0) -> tuple[int, int, str]:
    marker_index = source.index(marker, start)
    opening = source.index("{", marker_index)
    depth = 0
    for index in range(opening, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return marker_index, index + 1, source[opening + 1 : index]
    raise AssertionError(f"unterminated block after {marker!r}")


class SchedulerHotDiagnosticCleanupContractTests(unittest.TestCase):
    def test_cached_default_off_gates_use_only_explicit_diagnostics(self):
        timeline_gate = between(
            SCHEDULER,
            "bool ExecutorAndroidGpuTimelineEnabled()",
            "bool ExecutorFinishWaitProfileEnabled()",
        )
        self.assertIn("static const bool enabled", timeline_gate)
        self.assertIn("diag-vk-gpu-timeline", timeline_gate)

        finish_gate = between(
            SCHEDULER,
            "bool ExecutorFinishWaitProfileEnabled()",
            "bool ExecutorVulkanOptimizationStatsEnabled()",
        )
        self.assertIn("static const bool enabled", finish_gate)
        self.assertIn("ExecutorAndroidGpuTimelineEnabled()", finish_gate)
        self.assertIn("run-diagnostic-vk-finish-wait", finish_gate)
        self.assertIn("::access", finish_gate)
        self.assertNotIn("managedGpuFastPath", finish_gate)

        optimization_gate = between(
            SCHEDULER,
            "bool ExecutorVulkanOptimizationStatsEnabled()",
            "bool ExecutorSubmitMutexProfileEnabled()",
        )
        self.assertIn("static const bool enabled", optimization_gate)
        self.assertIn("run-diagnostic-vulkan-optimization-stats", optimization_gate)
        self.assertIn("::access", optimization_gate)
        self.assertNotIn("getenv", optimization_gate)
        self.assertEqual(
            SCHEDULER.count("run-diagnostic-vulkan-optimization-stats"), 1
        )

    def test_finish_clocks_counters_and_log_are_all_behind_one_gate(self):
        finish = between(
            SCHEDULER,
            "void Scheduler::Finish(const ExecutorFinishReason reason)",
            "void Scheduler::Wait(u64 tick)",
        )
        guard_start, guard_end, profile = braced_block(
            finish, "if (executor_finish_wait_profile_enabled)"
        )
        setup = finish[:guard_start]
        core = finish[guard_end:]

        self.assertIn("ExecutorFinishWaitProfileEnabled()", setup)
        self.assertIn("executor_finish_wait_profile_enabled", setup)
        self.assertIn("? std::chrono::steady_clock::now()", setup)
        self.assertIn("std::chrono::steady_clock::time_point{}", setup)
        self.assertEqual(finish.count("std::chrono::steady_clock::now()"), 2)
        for token in (
            "std::chrono::steady_clock::now() - executor_finish_begin",
            "executor_finish_wait_counts",
            "executor_finish_wait_ns",
            "executor_finish_wait_max_ns",
            "executor_finish_wait_total",
            "[EXECUTOR_VK_FINISH_WAIT]",
        ):
            self.assertIn(token, profile)
            self.assertNotIn(token, core)

        for functional in (
            "pending_buffer_transfers.empty()",
            "pending_ops_count.load",
            "PublishPendingSubmitBatch()",
            "master_semaphore.Wait",
            "SubmitExecution(",
            "Wait(",
        ):
            self.assertIn(functional, core)

    def test_transfer_telemetry_is_gated_but_transfer_state_and_commands_are_not(self):
        queue = between(
            SCHEDULER,
            "void Scheduler::QueueBufferCopies",
            "void Scheduler::QueueBufferUploadCopies",
        )
        gate_start, _, queue_stats = braced_block(
            queue, "if (ExecutorVulkanOptimizationStatsEnabled())"
        )
        for functional in (
            "VideoCore::RecordVulkanTransferRequest",
            "++pending_buffer_transfer_requests",
            "++pending_buffer_transfer_request_epoch",
            "pending_buffer_transfer_regions += regions.size()",
            "pending_buffer_transfer_origins |=",
            "pending_gnm_dma_transfers +=",
        ):
            self.assertIn(functional, queue[:gate_start])
            self.assertNotIn(functional, queue_stats)
        for diagnostic in (
            "executor_buffer_transfers_queued",
            "executor_buffer_transfer_regions_queued",
            "executor_buffer_transfer_max_queued",
            "executor_hle_buffer_transfers_queued",
            "executor_buffer_uploads_queued",
            "executor_gnm_dma_transfers_queued",
            "executor_gnm_dma_regions_queued",
        ):
            self.assertIn(diagnostic, queue_stats)

        flush = between(
            SCHEDULER,
            "void Scheduler::FlushPendingBufferTransfers()",
            "void Scheduler::Flush(SubmitInfo& info)",
        )
        self.assertIn(
            "const bool executor_vulkan_optimization_stats_enabled",
            flush,
        )
        dependency_start, dependency_end, dependency_stats = braced_block(
            flush, "if (executor_vulkan_optimization_stats_enabled)"
        )
        self.assertIn(
            "++executor_buffer_transfer_dependency_barriers", dependency_stats
        )
        dependency_context = flush[max(0, dependency_start - 350) : dependency_end + 180]
        self.assertIn("cmdbuf.pipelineBarrier", dependency_context)
        self.assertIn("cmdbuf.copyBuffer", dependency_context)

        summary_start, _, summary_stats = braced_block(
            flush,
            "if (executor_vulkan_optimization_stats_enabled)",
            dependency_end,
        )
        self.assertIn("[EXECUTOR_BUFFER_TRANSFER_BATCH]", summary_stats)
        self.assertIn("emitted_copy_regions", summary_stats)
        self.assertIn("executor_buffer_transfer_flushes", summary_stats)
        self.assertLess(flush.index("cmdbuf.copyBuffer"), summary_start)
        self.assertLess(flush.index("VideoCore::RecordVulkanTransferFlush"), summary_start)
        for functional in (
            "cmdbuf.copyBuffer",
            "cmdbuf.pipelineBarrier",
            "pending_buffer_transfers.clear()",
            "pending_buffer_transfer_requests = 0",
            "pending_gnm_dma_transfers = 0",
        ):
            self.assertNotIn(functional, summary_stats)
            self.assertIn(functional, flush)

    def test_submit_aggregate_is_gated_without_gating_queue_or_progress(self):
        submit = between(
            SCHEDULER,
            "bool Scheduler::QueueSubmitLocked(",
            "bool Scheduler::SubmitPendingBatchLocked(",
        )
        gate_start, gate_end, aggregate = braced_block(
            submit, "if (ExecutorVulkanOptimizationStatsEnabled())"
        )
        self.assertLess(submit.index("instance.GetGraphicsQueue().submit"), gate_start)
        self.assertLess(submit.index("VideoCore::RecordVulkanQueueSubmit"), gate_start)
        for token in (
            "executor_submit_batches",
            "executor_submit_logical_packets",
            "executor_submit_command_buffers",
            "executor_submit_calls_saved",
            "executor_submit_forced_boundaries",
            "executor_submit_timer_flushes",
            "executor_submit_max_batch",
            "[EXECUTOR_VK_SUBMIT_COALESCING]",
        ):
            self.assertIn(token, aggregate)
        for forbidden in (
            "GetGraphicsQueue().submit",
            "PublishRetirementTick",
            "MarkTerminal",
            "return false",
            "return true",
        ):
            self.assertNotIn(forbidden, aggregate)
        self.assertIn("PublishRetirementTick", submit[gate_end:])

        timer = between(
            SCHEDULER,
            "void Scheduler::PendingSubmitThread",
            "void Scheduler::SubmitExecution",
        )
        _, timer_gate_end, timer_stats = braced_block(
            timer, "if (ExecutorVulkanOptimizationStatsEnabled())"
        )
        self.assertIn("++executor_submit_timer_flushes", timer_stats)
        self.assertIn("SubmitPendingBatchLocked()", timer[timer_gate_end:])
        self.assertNotIn("SubmitPendingBatchLocked", timer_stats)

    def test_render_scope_history_is_explicit_diagnostic_only(self):
        begin = between(
            SCHEDULER,
            "void Scheduler::BeginRendering(const RenderState& new_state)",
            "void Scheduler::EndRendering(const EndRenderingReason reason)",
        )
        gate = begin.index("ExecutorVulkanOptimizationStatsEnabled()")
        history = begin.index("RenderScopeTelemetryFor(this)")
        self.assertLess(gate, history)
        self.assertIn("telemetry != nullptr", begin)
        self.assertIn("CanReuseAfterConsumedClear(render_state, new_state)", begin)
        self.assertIn("VideoCore::RecordVulkanRenderScopeReuse()", begin)

        end = between(
            SCHEDULER,
            "void Scheduler::EndRendering(const EndRenderingReason reason)",
            "void Scheduler::PublishRetirementTick",
        )
        for occurrence in (
            "RenderScopeTelemetryFor(this).has_last_ended_render_state = false",
            "auto& telemetry = RenderScopeTelemetryFor(this)",
        ):
            history = end.index(occurrence)
            gate = end.rfind("ExecutorVulkanOptimizationStatsEnabled()", 0, history)
            self.assertNotEqual(gate, -1)
            self.assertLess(history - gate, 220)

    def test_diagnostic_aggregates_are_per_scheduler_not_cross_instance_statics(self):
        fields = between(
            HEADER,
            "u64 executor_submit_kind_total{};",
            "#endif\n    RenderState render_state",
        )
        self.assertNotIn("inline static", fields)
        for name in (
            "executor_buffer_transfers_queued",
            "executor_buffer_transfer_regions_queued",
            "executor_buffer_transfer_flushes",
            "executor_buffer_transfer_copy_calls",
            "executor_buffer_transfer_copy_regions_emitted",
            "executor_buffer_transfer_barriers_requested",
            "executor_buffer_transfer_barriers_emitted",
            "executor_buffer_transfer_max_queued",
            "executor_buffer_transfer_dependency_barriers",
            "executor_hle_buffer_transfers_queued",
            "executor_buffer_uploads_queued",
            "executor_gnm_dma_transfers_queued",
            "executor_gnm_dma_regions_queued",
            "executor_submit_batches",
            "executor_submit_logical_packets",
            "executor_submit_command_buffers",
            "executor_submit_calls_saved",
            "executor_submit_forced_boundaries",
            "executor_submit_timer_flushes",
            "executor_submit_max_batch",
            "executor_submit_latency_samples",
            "executor_submit_latency_total_ns",
            "executor_submit_latency_max_ns",
        ):
            self.assertIn(f"u64 {name}{{}};", fields)
        self.assertNotIn("static u64 executor_submit_", SCHEDULER)


if __name__ == "__main__":
    unittest.main()
