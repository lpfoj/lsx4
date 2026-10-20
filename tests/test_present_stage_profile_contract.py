import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PRESENTER = (
    ROOT / "funnel-arm/src/video_core/renderer_vulkan/vk_presenter.cpp"
).read_text(encoding="utf-8")
OVERLAP_TRACE = (
    ROOT / "funnel-arm/src/video_core/invalidation_overlap_trace.cpp"
).read_text(encoding="utf-8")


def between(source: str, start: str, end: str) -> str:
    return source.split(start, 1)[1].split(end, 1)[0]


class PresenterStageProfileContractTests(unittest.TestCase):
    def test_profiler_is_explicit_marker_only_and_clock_is_gated(self):
        gate_start = PRESENTER.index("bool ExecutorPresentStageProfileEnabled()")
        gate_body = PRESENTER[gate_start : gate_start + 650]
        self.assertIn("run-diagnostic-present-stage-profile", gate_body)
        self.assertIn("static const bool enabled", gate_body)
        self.assertNotIn("getenv", gate_body)

        ctor_start = PRESENTER.index("explicit PresenterCpuStageProfile")
        ctor_body = PRESENTER[ctor_start : ctor_start + 260]
        self.assertIn("if (enabled_)", ctor_body)
        self.assertIn("ExecutorPresentStageNowNs()", ctor_body)

    def test_all_requested_cpu_stages_are_aggregated_every_120_frames(self):
        for token in (
            "PresenterCpuStage::Acquire",
            "PresenterCpuStage::CommandStart",
            "PresenterCpuStage::Record",
            "PresenterCpuStage::Flush",
            "PresenterCpuStage::QueuePresent",
            "acquire_avg_ms",
            "command_start_avg_ms",
            "record_avg_ms",
            "flush_avg_ms",
            "queue_present_avg_ms",
            "other_avg_ms",
        ):
            self.assertIn(token, PRESENTER)
        self.assertIn("constexpr u64 PublishInterval = 120", PRESENTER)
        self.assertIn("present-stage-profile.txt", PRESENTER)
        self.assertEqual(PRESENTER.count("[EXECUTOR_PRESENT_STAGE_PROFILE]"), 1)

    def test_only_routed_frames_that_reached_queue_present_are_sampled(self):
        commit_start = PRESENTER.index("void Commit(const u64 frame_ns) const")
        commit_body = PRESENTER[commit_start : commit_start + 1100]
        guard = (
            "route_ == PresenterCpuRoute::Unknown || !queue_present_called_"
        )
        self.assertIn(guard, commit_body)
        self.assertLess(commit_body.index(guard), commit_body.index("totals.stage_ns"))

    def test_windows_are_thread_safe_non_overlapping_batches(self):
        commit_start = PRESENTER.index("void Commit(const u64 frame_ns) const")
        commit_body = PRESENTER[commit_start : commit_start + 5200]
        self.assertIn("std::scoped_lock lock{totals.mutex}", commit_body)
        self.assertIn("const u64 samples = ++totals.samples", commit_body)
        self.assertIn("if (samples < PublishInterval)", commit_body)
        self.assertIn("totals.stage_ns.fill(0)", commit_body)
        self.assertIn("totals.route_samples.fill(0)", commit_body)
        self.assertIn("totals.total_ns = 0", commit_body)
        self.assertIn("totals.samples = 0", commit_body)
        self.assertIn("totals.queue_present_calls = 0", commit_body)
        self.assertIn("totals.queue_present_ok = 0", commit_body)

    def test_acquire_and_vk_queue_present_calls_are_directly_bracketed(self):
        acquire = PRESENTER.index(
            'AcquireSwapchainImage("videoout_acquire")'
        )
        acquire_body = PRESENTER[acquire - 180 : acquire + 220]
        self.assertIn("Begin(PresenterCpuStage::Acquire)", acquire_body)
        self.assertIn("End(PresenterCpuStage::Acquire)", acquire_body)

        normal_present = PRESENTER.rindex("const bool present_ok = swapchain.Present();")
        profile_begin = PRESENTER.rfind(
            "present_stage_profile.Begin(PresenterCpuStage::QueuePresent)",
            0,
            normal_present,
        )
        overlap_begin = PRESENTER.rfind(
            "InvalidationOverlapTrace::BeginPresentV2(",
            profile_begin,
            normal_present,
        )
        overlap_end = PRESENTER.find(
            "InvalidationOverlapTrace::EndPresentV2(", normal_present
        )
        profile_end = PRESENTER.find(
            "present_stage_profile.End(PresenterCpuStage::QueuePresent)",
            normal_present,
        )
        result = PRESENTER.find(
            "present_stage_profile.RecordQueuePresentResult(present_ok)",
            normal_present,
        )
        for position in (
            profile_begin,
            overlap_begin,
            overlap_end,
            profile_end,
            result,
        ):
            self.assertGreaterEqual(position, 0)
        self.assertLess(profile_begin, overlap_begin)
        self.assertLess(overlap_begin, normal_present)
        self.assertLess(normal_present, overlap_end)
        self.assertLess(overlap_end, profile_end)
        self.assertLess(profile_end, result)

        begin_hook = between(
            OVERLAP_TRACE,
            "PresentTokenV2 BeginPresentV2(",
            "void EndPresentV2(",
        )
        self.assertLess(
            begin_hook.index("if (frame_token == 0)"),
            begin_hook.index("std::chrono::steady_clock::now()"),
        )
        self.assertIn(
            "ActiveContext.load(std::memory_order_acquire) != &trace_storage_v2",
            begin_hook,
        )
        end_hook = between(
            OVERLAP_TRACE,
            "void EndPresentV2(",
            "} // namespace VideoCore::InvalidationOverlapTrace",
        )
        self.assertLess(
            end_hook.index("if (!token || token.context != &trace_storage_v2)"),
            end_hook.index("RecordEnabledV2("),
        )

    def test_existing_turnip_wait_stage_narrowing_is_preserved(self):
        anchor = PRESENTER.index('WaitForFrameReady(frame, "present_frame_ready")')
        body = PRESENTER[anchor - 700 : anchor + 700]
        self.assertIn("vk::PipelineStageFlagBits::eColorAttachmentOutput", body)
        self.assertIn("vk::PipelineStageFlagBits::eFragmentShader", body)


if __name__ == "__main__":
    unittest.main()
