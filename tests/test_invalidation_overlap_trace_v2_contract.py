from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
TRACE_HEADER = (
    ROOT / "funnel-arm/src/video_core/invalidation_overlap_trace.h"
).read_text(encoding="utf-8")
TRACE_CPP = (
    ROOT / "funnel-arm/src/video_core/invalidation_overlap_trace.cpp"
).read_text(encoding="utf-8")
RASTERIZER_H = (
    ROOT / "funnel-arm/src/video_core/renderer_vulkan/vk_rasterizer.h"
).read_text(encoding="utf-8")
RASTERIZER_CPP = (
    ROOT / "funnel-arm/src/video_core/renderer_vulkan/vk_rasterizer.cpp"
).read_text(encoding="utf-8")
SCHEDULER_CPP = (
    ROOT / "funnel-arm/src/video_core/renderer_vulkan/vk_scheduler.cpp"
).read_text(encoding="utf-8")
SCHEDULER_H = (
    ROOT / "funnel-arm/src/video_core/renderer_vulkan/vk_scheduler.h"
).read_text(encoding="utf-8")
PRESENTER_CPP = (
    ROOT / "funnel-arm/src/video_core/renderer_vulkan/vk_presenter.cpp"
).read_text(encoding="utf-8")
PRESENTER_H = (
    ROOT / "funnel-arm/src/video_core/renderer_vulkan/vk_presenter.h"
).read_text(encoding="utf-8")
VIDEOOUT_DRIVER = (
    ROOT / "funnel-arm/src/core/libraries/videoout/driver.cpp"
).read_text(encoding="utf-8")


def _between(source, start, end):
    return source.split(start, 1)[1].split(end, 1)[0]


class InvalidationOverlapTraceV2ContractTests(unittest.TestCase):
    def test_v1_abi_and_output_are_preserved(self):
        for token in (
            "StatsV1",
            "SnapshotHeaderV1",
            "SnapshotRecordV1",
            "executor_funnel_invalidation_overlap_trace_stats_v1",
            "executor_funnel_invalidation_overlap_trace_snapshot_v1",
            '"trace-v1.bin"',
        ):
            self.assertIn(token, TRACE_HEADER + TRACE_CPP)
        self.assertIn("TraceAbiVersion = 1", TRACE_CPP)
        self.assertIn("TraceFeatures = FeatureWaitSpans | FeatureDrainSpans", TRACE_CPP)

    def test_v2_is_explicit_default_off_and_has_a_distinct_snapshot(self):
        self.assertIn("EXECUTOR_PROFILE_GUEST_INVALIDATION_OVERLAP_V2", TRACE_CPP)
        self.assertIn('JoinPath(control_directory, "abi-v2")', TRACE_CPP)
        self.assertIn('"trace-v2.bin"', TRACE_CPP)
        self.assertIn("LSX4IOT2", TRACE_CPP.replace("'", "" ).replace(", ", ""))
        self.assertIn("executor_funnel_invalidation_overlap_trace_snapshot_v2", TRACE_HEADER)
        self.assertIn("FeatureV2CaptureEpoch", TRACE_HEADER)
        self.assertIn("FeatureV2InstrumentedPerturbation", TRACE_HEADER)
        self.assertIn("FeatureV2FrameOwnershipToken", TRACE_HEADER)

    def test_v2_request_identity_is_stored_on_the_request_and_mapped_by_drainer(self):
        self.assertIn("RequestTokenV2 overlap_trace_v2", RASTERIZER_H)
        self.assertIn("BeginRequestV2", RASTERIZER_CPP)
        self.assertIn("EndRequestV2", RASTERIZER_CPP)
        self.assertIn("BeginDrainBatchV2", RASTERIZER_CPP)
        self.assertIn("RecordRequestBatchMappingV2", RASTERIZER_CPP)
        self.assertIn("EndDrainBatchV2", RASTERIZER_CPP)
        self.assertLess(
            RASTERIZER_CPP.index("RecordRequestBatchMappingV2"),
            RASTERIZER_CPP.index("if (liverpool == nullptr || liverpool->IsRendererTerminal())", RASTERIZER_CPP.index("const auto process_batch")),
        )

    def test_only_concrete_frame_owned_presents_have_identity_hooks(self):
        self.assertIn("RecordFrameBoundaryV2", SCHEDULER_CPP)
        self.assertIn("ExecutorVkGpuTimelineReserveFrameToken", SCHEDULER_H)
        self.assertIn("closed_epoch", SCHEDULER_CPP)
        self.assertIn("overlap_trace_frame_token", PRESENTER_H)
        self.assertIn("frame->overlap_trace_frame_token", VIDEOOUT_DRIVER)
        self.assertLess(
            VIDEOOUT_DRIVER.index("ExecutorVkGpuTimelineReserveFrameToken"),
            VIDEOOUT_DRIVER.index("requests.push({"),
        )
        publish_window = VIDEOOUT_DRIVER.split("requests.push({", 1)[1].split(
            'ExecutorLogVideoOutPortState("internal_queued"', 1
        )[0]
        self.assertIn("ExecutorVkGpuTimelineFrameBoundary", publish_window)
        self.assertLess(
            publish_window.index("});"),
            publish_window.index("ExecutorVkGpuTimelineFrameBoundary"),
        )
        replay = _between(
            PRESENTER_CPP,
            "bool Presenter::ExecutorPresentReplayRT()",
            "\nvoid Presenter::ExecutorRecyclePreparedFrame",
        )
        self.assertIn("swapchain.Present()", replay)
        self.assertNotIn("BeginPresentV2", replay)
        frame_present = PRESENTER_CPP.split("void Presenter::Present(Frame* frame", 1)[1]
        self.assertEqual(frame_present.count("BeginPresentV2("), 3)
        self.assertEqual(frame_present.count("EndPresentV2("), 3)
        self.assertEqual(
            frame_present.count("frame->overlap_trace_frame_token"), 3
        )

    def test_saturation_quiescence_and_mapping_fail_closed(self):
        for token in (
            "SaturateV2",
            "coverage_exact.store(0",
            "epoch_mapping_exact.store(0",
            "active_spans",
            "ControllerMaximumQuiesce",
            "left_censored_requests",
            "right_censored_requests",
            "mixed_generation_batches",
            "epoch_crossing_batches",
            "structuralMappingExact",
            "structuralVerdict=parser-required",
            "delayCausality=not-claimed",
            "productionFrametime=0",
        ):
            self.assertIn(token, TRACE_CPP)

    def test_stale_v2_control_files_are_removed_before_arm_and_epoch_is_published(self):
        controller = _between(
            TRACE_CPP,
            "void ControllerMainV2(const std::string& control_directory)",
            "\nvoid ControllerEntry(",
        )
        for token in (
            "stale_paths",
            "RemoveIfPresent(stale_path)",
            'output_path + ".tmp"',
            "RemoveIfPresent(arm_path)",
            '" captureEpoch="',
        ):
            self.assertIn(token, controller)
        self.assertLess(controller.index("stale_paths"), controller.index("while (!PathExists(arm_path))"))
        self.assertLess(controller.index("RemoveIfPresent(arm_path)"), controller.index("ResetAndPrefaultV2"))

    def test_disabled_hot_request_path_keeps_existing_single_outer_gate(self):
        request_prefix = RASTERIZER_CPP.split("GuestInvalidationRequest request{", 1)[0]
        request_prefix = request_prefix.rsplit(
            "bool Rasterizer::InvalidateMemoryFromGuestThread(", 1
        )[1]
        self.assertEqual(
            request_prefix.count("InvalidationOverlapTrace::Enabled()"), 1
        )
        begin_function = TRACE_CPP.split("RequestTokenV2 BeginRequestV2", 1)[1].split(
            "\n}\n", 1
        )[0]
        self.assertLess(
            begin_function.index("ActiveContext.load"),
            begin_function.index("steady_clock::now")
            if "steady_clock::now" in begin_function
            else len(begin_function),
        )


if __name__ == "__main__":
    unittest.main()
