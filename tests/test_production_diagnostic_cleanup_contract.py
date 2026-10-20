import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SCHEDULER = (
    ROOT / "funnel-arm/src/video_core/renderer_vulkan/vk_scheduler.cpp"
).read_text(encoding="utf-8")
SCHEDULER_HEADER = (
    ROOT / "funnel-arm/src/video_core/renderer_vulkan/vk_scheduler.h"
).read_text(encoding="utf-8")
RASTERIZER = (
    ROOT / "funnel-arm/src/video_core/renderer_vulkan/vk_rasterizer.cpp"
).read_text(encoding="utf-8")


def between(source: str, start: str, end: str) -> str:
    return source.split(start, 1)[1].split(end, 1)[0]


class ProductionDiagnosticCleanupContractTests(unittest.TestCase):
    def test_journal_gate_honors_every_explicit_source_and_defaults_off(self):
        self.assertIn(
            "ExecutorVkJournalWouldRecord(bool force_record = false) noexcept",
            SCHEDULER_HEADER,
        )
        persistent = between(
            SCHEDULER,
            "bool ExecutorVkPersistentJournalRequested()",
            "bool ExecutorVkJournalRecordEnabled",
        )
        self.assertIn("static const bool persistent_journal", persistent)
        self.assertEqual(persistent.count('std::getenv("EXECUTOR_VK_JOURNAL")'), 1)

        record_gate = between(
            SCHEDULER,
            "bool ExecutorVkJournalRecordEnabled",
            "template <typename Fill>",
        )
        self.assertIn(
            "executor_vk_forced_session.store(true, std::memory_order_release)",
            record_gate,
        )
        self.assertIn("return ExecutorVkJournalWouldRecord(force_record)", record_gate)

        outer_gate = between(
            SCHEDULER,
            "bool ExecutorVkJournalWouldRecord",
            "u64 ExecutorVkJournalGraphicsDraw",
        )
        force = outer_gate.index("if (force_record)")
        persistent = outer_gate.index("if (ExecutorVkPersistentJournalRequested())")
        forced_session = outer_gate.index("executor_vk_forced_session.load")
        frame_capture = outer_gate.index("executor_vk_frame_capture_active.load")
        self.assertLess(force, persistent)
        self.assertLess(persistent, forced_session)
        self.assertLess(forced_session, frame_capture)
        self.assertEqual(outer_gate.count("return true;"), 3)
        self.assertIn(
            "return executor_vk_frame_capture_active.load(std::memory_order_acquire)",
            outer_gate,
        )
        self.assertNotIn("managedGpuFastPath", outer_gate)
        self.assertNotIn("getenv", outer_gate)

    def test_force_record_latches_session_for_submit_begin_and_completion(self):
        record_gate = between(
            SCHEDULER,
            "bool ExecutorVkJournalRecordEnabled",
            "template <typename Fill>",
        )
        force = record_gate.index("if (force_record)")
        latch = record_gate.index(
            "executor_vk_forced_session.store(true, std::memory_order_release)"
        )
        consume = record_gate.index(
            "return ExecutorVkJournalWouldRecord(force_record)"
        )
        self.assertLess(force, latch)
        self.assertLess(latch, consume)

        outer_gate = between(
            SCHEDULER,
            "bool ExecutorVkJournalWouldRecord",
            "u64 ExecutorVkJournalGraphicsDraw",
        )
        self.assertIn("executor_vk_forced_session.load", outer_gate)
        self.assertNotIn("managedGpuFastPath", outer_gate)

        submit = between(
            SCHEDULER,
            "bool Scheduler::QueueSubmitLocked(",
            "bool Scheduler::SubmitPendingBatchLocked(",
        )
        active = submit.index(
            "const bool executor_journal_active = ExecutorVkJournalWouldRecord()"
        )
        begin_guard = submit.index("if (executor_journal_active)", active)
        begin = submit.index(
            "ExecutorVkJournalSubmit(this, tick, false, vk::Result::eSuccess)",
            begin_guard,
        )
        complete_guard = submit.index("if (executor_journal_active)", begin)
        complete = submit.index(
            "ExecutorVkJournalSubmit(this, tick, true, submit_result)",
            complete_guard,
        )
        self.assertLess(active, begin_guard)
        self.assertLess(begin_guard, begin)
        self.assertLess(begin, complete_guard)
        self.assertLess(complete_guard, complete)
        self.assertEqual(submit.count("if (executor_journal_active)"), 2)

    def test_submit_journal_and_latency_have_independent_exact_gates(self):
        submit = between(
            SCHEDULER,
            "bool Scheduler::QueueSubmitLocked(",
            "bool Scheduler::SubmitPendingBatchLocked(",
        )
        self.assertIn(
            "const bool executor_journal_active = ExecutorVkJournalWouldRecord()",
            submit,
        )
        self.assertIn(
            "executor_journal_active || ExecutorSubmitLatencyTraceEnabled()",
            submit,
        )
        self.assertIn(
            "executor_submit_latency_trace ? std::chrono::steady_clock::now()",
            submit,
        )
        self.assertEqual(submit.count("if (executor_journal_active)"), 2)
        self.assertIn("if (executor_submit_latency_trace)", submit)
        self.assertNotIn("executor_diagnostics_enabled", submit)
        self.assertNotIn("ExecutorVkJournalRequested", submit)

        latency_gate = between(
            SCHEDULER,
            "bool ExecutorSubmitLatencyTraceEnabled()",
            "u64 ExecutorSubmitMutexProfileNowNs()",
        )
        self.assertIn("static const bool enabled", latency_gate)
        self.assertIn("run-diagnostic-submit-latency-trace", latency_gate)
        self.assertIn("::access", latency_gate)

    def test_submit_reason_counters_and_log_are_marker_only(self):
        reason_gate = between(
            SCHEDULER,
            "bool ExecutorSubmitReasonsEnabled()",
            "bool ExecutorSubmitLatencyTraceEnabled()",
        )
        self.assertIn("static const bool enabled", reason_gate)
        self.assertIn("run-diagnostic-submit-reasons", reason_gate)
        self.assertIn("::access", reason_gate)
        self.assertNotIn("getenv", reason_gate)

        submit_execution = between(
            SCHEDULER,
            "void Scheduler::SubmitExecution(",
            "void Scheduler::PriorityPendingOpsThread(",
        )
        reason_block = between(
            submit_execution,
            "const bool executor_submit_reasons_enabled",
            "const bool managed_submit_coalescing",
        )
        self.assertIn("ExecutorSubmitReasonsEnabled()", reason_block)
        self.assertIn("++executor_submit_kind_counts", reason_block)
        self.assertIn("++executor_finish_reason_counts", reason_block)
        self.assertIn("[EXECUTOR_VK_SUBMIT_REASONS]", reason_block)
        self.assertNotIn("managedGpuFastPath", reason_block)

    def test_draw_and_compute_outer_journal_gates_preserve_force_probe(self):
        methods = (
            between(RASTERIZER, "void Rasterizer::Draw(bool", "void Rasterizer::DrawIndirect"),
            between(RASTERIZER, "void Rasterizer::DrawIndirect", "void Rasterizer::DispatchDirect"),
            between(RASTERIZER, "void Rasterizer::DispatchDirect", "void Rasterizer::DispatchIndirect"),
            between(RASTERIZER, "void Rasterizer::DispatchIndirect", "void Rasterizer::ScopeMarkerBegin"),
        )
        for method in methods:
            journal = method[method.index("const bool executor_force_journal") :]
            self.assertIn(
                "if (ExecutorVkJournalWouldRecord(executor_force_journal))",
                journal,
            )
            self.assertIn("executor_force_journal);", journal)
            self.assertNotIn("ExecutorVkJournalRequested", journal)

        self.assertEqual(
            RASTERIZER.count(
                "if (ExecutorVkJournalWouldRecord(executor_force_journal))"
            ),
            4,
        )

        render_commands = (
            (methods[0], "cmdbuf.drawIndexed("),
            (methods[1], "cmdbuf.drawIndexedIndirect("),
            (methods[2], "cmdbuf.dispatch("),
            (methods[3], "cmdbuf.dispatchIndirect("),
        )
        for method, command in render_commands:
            self.assertLess(
                method.index(command),
                method.index("if (ExecutorVkJournalWouldRecord"),
            )

    def test_videoout_source_work_uses_only_cached_env_gate(self):
        gate = between(
            RASTERIZER,
            "static bool ExecutorTraceVideoOutSourceEnabled()",
            "static bool ExecutorAdaptiveVrsProfileEnabled()",
        )
        self.assertIn("static const bool enabled", gate)
        self.assertEqual(gate.count('std::getenv("EXECUTOR_TRACE_VIDEOOUT_SOURCE")'), 1)

        outer_calls = 0
        for occurrence in (
            index
            for index in range(len(RASTERIZER))
            if RASTERIZER.startswith("ExecutorTraceDrawnColorTarget(", index)
        ):
            context = RASTERIZER[max(0, occurrence - 180) : occurrence]
            if "static void " in context:
                continue
            outer_calls += 1
            self.assertIn("if (ExecutorTraceVideoOutSourceEnabled())", context)
            self.assertNotIn("ExecutorVkJournal", context)
        self.assertEqual(outer_calls, 2)

    def test_adaptive_vrs_profile_gate_does_not_change_rate_decision(self):
        gate = between(
            RASTERIZER,
            "static bool ExecutorAdaptiveVrsProfileEnabled()",
            "struct ExecutorGuestRangeFingerprint",
        )
        self.assertIn("static const bool enabled", gate)
        self.assertIn("run-diagnostic-adaptive-vrs-profile", gate)
        self.assertNotIn("getenv", gate)

        update = between(
            RASTERIZER,
            "void Rasterizer::UpdateFragmentShadingRate",
            "void Rasterizer::UpdateViewportScissorState",
        )
        profile_start = update.index("if (ExecutorAdaptiveVrsProfileEnabled())")
        decision = update.index("use_coarse_rate =")
        record = update.index("VideoCore::RecordMobileGpuShadingDecision")
        apply_rate = update.index("SetFragmentShadingRate(rate)")
        self.assertLess(decision, profile_start)
        self.assertLess(profile_start, record)
        self.assertLess(record, apply_rate)
        profile_body = update[profile_start:record]
        self.assertIn("++total_draws", profile_body)
        self.assertIn("coarse_draws += use_coarse_rate", profile_body)
        self.assertIn("[EXECUTOR_ADAPTIVE_VRS]", profile_body)
        self.assertNotIn("rate =", profile_body)
        self.assertNotIn("return", profile_body)
        self.assertEqual(update.count("RecordMobileGpuShadingDecision"), 1)
        self.assertEqual(update.count("SetFragmentShadingRate(rate)"), 1)


if __name__ == "__main__":
    unittest.main()
