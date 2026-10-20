import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SCHEDULER = (
    ROOT / "funnel-arm/src/video_core/renderer_vulkan/vk_scheduler.cpp"
).read_text(encoding="utf-8")
SCHEDULER_HEADER = (
    ROOT / "funnel-arm/src/video_core/renderer_vulkan/vk_scheduler.h"
).read_text(encoding="utf-8")


def source_between(start: str, end: str) -> str:
    return SCHEDULER.split(start, 1)[1].split(end, 1)[0]


class SubmitMutexProfileContractTests(unittest.TestCase):
    def test_profiler_is_explicit_marker_only(self):
        gate = source_between(
            "bool ExecutorSubmitMutexProfileEnabled()",
            "u64 ExecutorSubmitMutexProfileNowNs()",
        )
        self.assertIn("run-diagnostic-submit-mutex-profile", gate)
        self.assertIn("static const bool enabled", gate)
        self.assertIn("::access", gate)
        self.assertNotIn("getenv", gate)
        self.assertNotIn("ExecutorAndroidRendererProbesEnabled", gate)
        self.assertNotIn("ExecutorAndroidGpuTimelineEnabled", gate)

    def test_lock_wait_and_hold_clocks_are_marker_gated(self):
        constructor = source_between(
            "explicit SubmitMutexProfileLock", "~SubmitMutexProfileLock()"
        )
        wait_clock = constructor.index(
            "wait_begin_ns = ExecutorSubmitMutexProfileNowNs()"
        )
        acquire = constructor.index("lock.lock()")
        acquired_clock = constructor.index(
            "acquired_ns = ExecutorSubmitMutexProfileNowNs()"
        )
        self.assertLess(constructor.rfind("if (enabled)", 0, wait_clock), wait_clock)
        self.assertLess(wait_clock, acquire)
        self.assertLess(acquire, acquired_clock)
        self.assertLess(
            constructor.rfind("if (enabled)", acquire, acquired_clock),
            acquired_clock,
        )
        self.assertNotIn("try_lock", constructor)
        self.assertNotIn("sleep", constructor)

        destructor = source_between(
            "~SubmitMutexProfileLock()", "SubmitMutexProfileLock(const"
        )
        hold_end = destructor.index("hold_end_ns = ExecutorSubmitMutexProfileNowNs()")
        snapshot = destructor.index("FinishSubmitMutexProfileLockLocked")
        unlock = destructor.index("lock.unlock()")
        publish = destructor.index("PublishSubmitMutexProfileSnapshot")
        self.assertLess(hold_end, snapshot)
        self.assertLess(snapshot, unlock)
        self.assertLess(unlock, publish)

    def test_publish_is_outside_submit_lock_and_windows_reset(self):
        aggregation = source_between(
            "FinishSubmitMutexProfileLockLocked", "const char* GpuTimelineKindName"
        )
        self.assertIn("SubmitMutexProfilePhysicalSubmitWindow", aggregation)
        self.assertIn("counters = {}", aggregation)
        self.assertNotIn("fopen", aggregation)
        self.assertNotIn("__android_log", aggregation)

        publisher = source_between(
            "void PublishSubmitMutexProfileSnapshot", "class SubmitMutexProfileLock"
        )
        self.assertIn('"schema=2 window=', publisher)
        self.assertIn("submit-mutex-profile.txt", publisher)
        self.assertIn("std::fopen", publisher)
        self.assertIn("[EXECUTOR_VK_SUBMIT_MUTEX_PROFILE]", publisher)
        self.assertEqual(SCHEDULER.count("[EXECUTOR_VK_SUBMIT_MUTEX_PROFILE]"), 1)

    def test_all_scheduler_kinds_and_requested_counters_are_reported(self):
        for token in (
            "GpuTimelineSchedulerKind::GuestDraw",
            "GpuTimelineSchedulerKind::Present",
            "GpuTimelineSchedulerKind::RepeatFlip",
            "lock_acquisitions",
            "physical_submits",
            "work_batches",
            "logical_packets",
            "command_buffers",
            "wait_total_ns",
            "wait_max_ns",
            "hold_total_ns",
            "hold_max_ns",
            "vk_queue_submit_total_ns",
            "vk_queue_submit_max_ns",
        ):
            self.assertIn(token, SCHEDULER)
        self.assertIn("SubmitMutexProfilePhysicalSubmitWindow = 120", SCHEDULER)

    def test_submit_execution_uses_one_profile_lock_for_whole_section(self):
        body = source_between(
            "void Scheduler::SubmitExecution", "void Scheduler::PriorityPendingOpsThread"
        )
        lock = body.index("SubmitMutexProfileLock lk{submit_mutex, gpu_timeline_kind}")
        terminal = body.index("master_semaphore.IsDeviceLost()")
        self.assertLess(lock, terminal)
        self.assertEqual(body.count("SubmitMutexProfileLock lk"), 1)
        for token in (
            "FlushPendingBufferTransfers()",
            "EndRendering(",
            "QueueSubmitLocked(",
            "SubmitPendingBatchLocked(",
            "AllocateWorkerCommandBuffers()",
            "PopPendingOperations(true, true)",
        ):
            self.assertIn(token, body)
        self.assertNotIn("lk.unlock()", body)

    def test_each_physical_queue_submit_is_directly_bracketed(self):
        body = source_between(
            "bool Scheduler::QueueSubmitLocked", "bool Scheduler::SubmitPendingBatchLocked"
        )

        normal_call = "instance.GetGraphicsQueue().submit(submits, tracking_fence)"
        normal = body.index(normal_call)
        normal_begin = body.rfind("submit_mutex_profile_begin_ns", 0, normal)
        normal_end = body.index("submit_mutex_profile_end_ns", normal)
        normal_record = body.index(
            "RecordSubmitMutexProfileQueueSubmitLocked", normal_end
        )
        self.assertLess(normal_begin, normal)
        self.assertLess(normal, normal_end)
        self.assertLess(normal_end, normal_record)

        empty_call = "instance.GetGraphicsQueue().submit(vk::SubmitInfo{}, fence)"
        empty = body.index(empty_call)
        empty_begin = body.rfind("empty_submit_profile_begin_ns", 0, empty)
        empty_end = body.index("empty_submit_profile_end_ns", empty)
        empty_record = body.index(
            "RecordSubmitMutexProfileQueueSubmitLocked", empty_end
        )
        self.assertLess(empty_begin, empty)
        self.assertLess(empty, empty_end)
        self.assertLess(empty_end, empty_record)

        self.assertIn(
            "submit_mutex_profile_enabled ? ExecutorSubmitMutexProfileNowNs() : 0",
            body,
        )
        self.assertIn("if (submit_mutex_profile_enabled)", body)

    def test_timer_reacquire_excludes_condition_wait_and_publishes_unlocked(self):
        adapter = source_between(
            "class SubmitMutexProfileReacquireMutex", "std::atomic<u64>"
        )
        wait_begin = adapter.index(
            "enabled ? ExecutorSubmitMutexProfileNowNs() : 0"
        )
        acquire = adapter.index("mutex.lock()")
        acquired_clock = adapter.index(
            "acquired_ns = ExecutorSubmitMutexProfileNowNs()"
        )
        self.assertLess(wait_begin, acquire)
        self.assertLess(acquire, acquired_clock)
        self.assertIn("if (enabled)", adapter)
        self.assertNotIn("condition_variable", adapter)

        timer = source_between(
            "void Scheduler::PendingSubmitThread", "void Scheduler::SubmitExecution"
        )
        self.assertIn(
            "SubmitMutexProfileReacquireMutex profiled_submit_mutex{submit_mutex}",
            timer,
        )
        self.assertIn("std::unique_lock lk{profiled_submit_mutex}", timer)
        self.assertIn("pending_submit_cv.wait(", timer)
        self.assertIn("pending_submit_cv.wait_until(lk, deadline)", timer)
        submit = timer.index("SubmitPendingBatchLocked()")
        hold_end = timer.index("hold_end_ns = ExecutorSubmitMutexProfileNowNs()")
        finish = timer.index("FinishSubmitMutexProfileLockLocked")
        unlock = timer.index("lk.unlock()", finish)
        publish = timer.index("PublishSubmitMutexProfileSnapshot", unlock)
        relock = timer.index("lk.lock()", publish)
        self.assertLess(submit, hold_end)
        self.assertLess(hold_end, finish)
        self.assertLess(finish, unlock)
        self.assertLess(unlock, publish)
        self.assertLess(publish, relock)
        self.assertNotIn("std::unique_lock lk{submit_mutex}", timer)

    def test_draw_submit_execution_stages_are_marker_gated(self):
        stage_scope = source_between(
            "class SubmitExecutionProfileStageScope",
            "void RecordSubmitExecutionProfileLocked",
        )
        begin_clock = stage_scope.index(
            "begin_ns = ExecutorSubmitMutexProfileNowNs()"
        )
        self.assertLess(stage_scope.index("if (sample.enabled)"), begin_clock)
        end_clock = stage_scope.index(
            "ExecutorSubmitMutexProfileNowNs() - begin_ns"
        )
        self.assertLess(stage_scope.index("if (!sample.enabled)"), end_clock)

        commit = source_between(
            "void RecordSubmitExecutionProfileLocked",
            "void RecordSubmitMutexProfileQueueSubmitLocked",
        )
        guard = commit.index("if (!sample.enabled)")
        clock = commit.index("ExecutorSubmitMutexProfileNowNs()")
        counter = commit.index("++counters.submit_execution_calls")
        self.assertLess(guard, clock)
        self.assertLess(clock, counter)
        self.assertIn("hold_ns > accounted_ns ? hold_ns - accounted_ns : 0", commit)

        body = source_between(
            "void Scheduler::SubmitExecution", "void Scheduler::PriorityPendingOpsThread"
        )
        self.assertIn(".enabled = lk.Enabled() &&", body)
        self.assertIn(
            "gpu_timeline_kind == GpuTimelineSchedulerKind::GuestDraw", body
        )
        self.assertIn("SCOPE_EXIT", body)
        self.assertLess(
            body.index("RecordSubmitExecutionProfileLocked"),
            body.index("master_semaphore.IsDeviceLost()"),
        )

    def test_all_requested_submit_execution_stages_are_directly_scoped(self):
        body = source_between(
            "void Scheduler::SubmitExecution", "void Scheduler::PriorityPendingOpsThread"
        )
        stage_tokens = (
            "SubmitExecutionProfileStage::FlushPendingBufferTransfers",
            "SubmitExecutionProfileStage::EndRenderingAndCommandBufferEnd",
            "SubmitExecutionProfileStage::ImGuiTextureSubmit",
            "SubmitExecutionProfileStage::QueueSubmitPath",
            "SubmitExecutionProfileStage::AllocateWorkerCommandBuffers",
            "SubmitExecutionProfileStage::PopPendingOperations",
        )
        for token in stage_tokens:
            self.assertIn(token, body)

        self.assertLess(
            body.index("SubmitExecutionProfileStage::FlushPendingBufferTransfers"),
            body.index("FlushPendingBufferTransfers()"),
        )
        end_stage = body.index(
            "SubmitExecutionProfileStage::EndRenderingAndCommandBufferEnd"
        )
        self.assertLess(end_stage, body.index("EndRendering(", end_stage))
        self.assertLess(end_stage, body.index("current_cmdbuf.end()", end_stage))
        imgui_stage = body.index("SubmitExecutionProfileStage::ImGuiTextureSubmit")
        self.assertLess(
            imgui_stage,
            body.index("ImGui::Core::TextureManager::Submit()", imgui_stage),
        )
        self.assertEqual(
            body.count("SubmitExecutionProfileStage::QueueSubmitPath"), 2
        )
        self.assertGreaterEqual(
            body.count("SubmitExecutionProfileStage::AllocateWorkerCommandBuffers"),
            2,
        )
        self.assertGreaterEqual(
            body.count("SubmitExecutionProfileStage::PopPendingOperations"), 2
        )

    def test_submit_kind_policy_and_route_counts_are_in_window_output(self):
        enum_body = SCHEDULER_HEADER.split("enum class ExecutorSubmitKind", 1)[1].split(
            "};", 1
        )[0]
        positions = [
            enum_body.index(name)
            for name in ("FlushInfo", "FlushPlain", "Finish", "WaitCurrent")
        ]
        self.assertEqual(positions, sorted(positions))

        for token in (
            "submit_kind_calls",
            "submit_external_sync_calls",
            "submit_can_defer_calls",
            "submit_deferred_calls",
            "submit_direct_calls",
            "submit_pending_batch_calls",
            "submit_forced_boundary_calls",
            "submit_execution_hold_total_ns",
            "submit_remainder_total_ns",
            "flushInfo=%llu",
            "flushPlain=%llu",
            "finish=%llu",
            "waitCurrent=%llu",
            "externalSync=%llu",
            "canDefer=%llu",
            "deferred=%llu",
            "directSubmit=%llu",
            "pendingBatchSubmit=%llu",
            "submitForcedBoundary=%llu",
            "flushTransfersTotalMs=%.3f",
            "endRenderEndTotalMs=%.3f",
            "imguiSubmitTotalMs=%.3f",
            "queueSubmitPathTotalMs=%.3f",
            "allocateWorkerTotalMs=%.3f",
            "popPendingTotalMs=%.3f",
            "remainderTotalMs=%.3f",
        ):
            self.assertIn(token, SCHEDULER)


if __name__ == "__main__":
    unittest.main()
