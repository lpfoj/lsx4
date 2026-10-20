import sys
import unittest
from pathlib import Path


TOOLS = Path(__file__).resolve().parents[1] / "tools"
sys.path.insert(0, str(TOOLS))

import check_vulkan_capability_bridge as guard


ROOT = Path(__file__).resolve().parents[1]


class VulkanCapabilityGuardTests(unittest.TestCase):
    def test_rejects_raw_promoted_commands(self):
        for command in (
            "vkCmdBeginRenderingKHR(command_buffer, &info);",
            "vkCmdEndRendering(command_buffer);",
            "vkCmdPipelineBarrier2KHR(command_buffer, &dependency);",
            "vkCmdPushDescriptorSetKHR(command_buffer, bind_point, layout, 0, 1, &write);",
        ):
            self.assertIsNotNone(guard.PROMOTED_RAW_CALL.search(command), command)

    def test_rejects_mobile_driver_branches(self):
        self.assertIsNotNone(
            guard.MOBILE_DRIVER_ENUM.search(
                "instance.GetDriverID() == vk::DriverId::eQualcommProprietary"
            )
        )
        self.assertIsNotNone(guard.MOBILE_VENDOR_LITERAL.search("vendorID == 0x13B5"))
        self.assertIsNotNone(guard.MOBILE_VENDOR_LITERAL.search("vendorID == 0x5143"))

    def test_tracks_route_aware_methods(self):
        samples = {
            ".beginRendering(rendering_info)": "beginRendering",
            ".endRendering()": "endRendering",
            ".pushDescriptorSetKHR(bind_point, layout, 0, writes)": "pushDescriptorSetKHR",
        }
        for source, name in samples.items():
            self.assertTrue(
                any(pattern.search(source) for pattern in guard.METHOD_ALLOWLIST), name
            )

    def test_auxiliary_shader_contract_is_guarded(self):
        tokens = guard.REQUIRED_TOKENS[
            "shader_recompiler/backend/spirv/emit_spirv_quad_rect.cpp"
        ]
        self.assertIn("supported_spirv", tokens)
        self.assertIn("std::clamp(supported_spirv", tokens)

    def test_a750_conservative_image_stages_are_exact_and_profile_routed(self):
        profile = (ROOT / "funnel-arm/src/video_core/renderer_vulkan/vk_capability_profile.h").read_text(
            encoding="utf-8"
        )
        instance = (ROOT / "funnel-arm/src/video_core/renderer_vulkan/vk_instance.cpp").read_text(
            encoding="utf-8"
        )
        image = (ROOT / "funnel-arm/src/video_core/texture_cache/image.cpp").read_text(
            encoding="utf-8"
        )
        self.assertIn("CapabilityQuirk::ConservativeImageTransitionStages", profile)
        self.assertIn("ExactProprietaryAdreno750Quirks(", profile)
        self.assertIn("0x44050a31, true, true, true, true) == 0", profile)
        self.assertIn("MobileDriverFamily::MesaTurnip, QualcommVendorId", profile)
        self.assertIn("run-sm8650-a750-conservative-image-stages-ab", instance)
        self.assertIn("ExactProprietaryAdreno750Quirks(", instance)
        self.assertIn(
            "HasQuirk(\n            CapabilityQuirk::ConservativeImageTransitionStages)",
            image,
        )
        self.assertNotIn("Adreno750DeviceId", image)
        self.assertIn("CapabilityQuirk::ExplicitFullImageSubresourceCounts", image)
        self.assertIn("run-sm8650-a750-explicit-image-subresources-ab", instance)
        self.assertIn("CapabilityQuirk::ReadOnlyDepthStencilAccess", profile)
        self.assertIn("run-sm8650-a750-readonly-depth-access-ab", instance)
        self.assertIn("dst_mask & transfer_access_mask", image)
        self.assertIn("dst_mask & ~transfer_access_mask", image)

    def test_a750_residency_telemetry_is_explicit_diagnostic_only(self):
        profile = (ROOT / "funnel-arm/src/video_core/renderer_vulkan/vk_capability_profile.h").read_text(
            encoding="utf-8"
        )
        instance = (ROOT / "funnel-arm/src/video_core/renderer_vulkan/vk_instance.cpp").read_text(
            encoding="utf-8"
        )
        buffer_cache = (ROOT / "funnel-arm/src/video_core/buffer_cache/buffer_cache.cpp").read_text(
            encoding="utf-8"
        )
        texture_cache = (ROOT / "funnel-arm/src/video_core/texture_cache/texture_cache.cpp").read_text(
            encoding="utf-8"
        )
        field = "diagnostic_a750_residency_telemetry"
        self.assertIn(field, profile)
        self.assertIn("run-diagnostic-sm8650-a750-residency", instance)
        self.assertIn("EnableExactA750TurnipD9ResidencyTelemetry", profile)
        self.assertIn(
            "GpuCompatibilityRoute::Adreno750MesaTurnip", profile
        )
        self.assertIn(
            "MobileDriverBuild::MesaTurnipA750Da14d65e44Ib32RolloverBoStats",
            profile,
        )
        self.assertIn(
            "EnableExactA750TurnipD9ResidencyTelemetry(\n"
            "            capability_profile.gpu_compatibility_route,\n"
            "            capability_profile.mobile_driver_build,\n"
            "            a750_residency_telemetry_requested)",
            instance,
        )
        self.assertIn(field, buffer_cache)
        self.assertIn(field, texture_cache)
        self.assertNotIn(
            "instance.GetCapabilityProfile().exact_proprietary_adreno750;",
            buffer_cache,
        )
        self.assertNotIn(
            "instance.GetCapabilityProfile().exact_proprietary_adreno750;",
            texture_cache,
        )

    def test_a750_runtime_barrier_quirks_do_not_cold_start_pipeline_storage(self):
        profile = (ROOT / "funnel-arm/src/video_core/renderer_vulkan/vk_capability_profile.h").read_text(
            encoding="utf-8"
        )
        serialization = (ROOT / "funnel-arm/src/video_core/renderer_vulkan/vk_pipeline_serialization.cpp").read_text(
            encoding="utf-8"
        )
        for quirk in (
            "ConservativeImageTransitionStages",
            "ExplicitFullImageSubresourceCounts",
            "ReadOnlyDepthStencilAccess",
            "SplitDepthStencilImageBarriers",
            "RequireStorageBufferReadWriteBarrier",
        ):
            self.assertIn(
                f"static_cast<u64>(CapabilityQuirk::{quirk})",
                profile.split("constexpr u64 RuntimeOnlyBarrierQuirks", 1)[1].split(
                    "constexpr u64 PipelineStorageDriverQuirks", 1
                )[0],
            )
        self.assertIn(
            "PipelineStorageDriverQuirks(capability.driver_quirks)",
            serialization,
        )
        self.assertNotIn(
            "fingerprint.UpdateLittleEndian(capability.driver_quirks);",
            serialization,
        )

    def test_a750_rw_storage_barrier_is_exact_and_preserves_other_driver_paths(self):
        profile = (ROOT / "funnel-arm/src/video_core/renderer_vulkan/vk_capability_profile.h").read_text(
            encoding="utf-8"
        )
        instance = (ROOT / "funnel-arm/src/video_core/renderer_vulkan/vk_instance.cpp").read_text(
            encoding="utf-8"
        )
        buffer = (ROOT / "funnel-arm/src/video_core/buffer_cache/buffer.h").read_text(
            encoding="utf-8"
        )
        rasterizer = (ROOT / "funnel-arm/src/video_core/renderer_vulkan/vk_rasterizer.cpp").read_text(
            encoding="utf-8"
        )
        self.assertIn("ExactProprietaryAdreno750RuntimeQuirks", profile)
        self.assertIn("0x44050a31) == 0", profile)
        self.assertIn(
            "MobileDriverFamily::MesaTurnip, QualcommVendorId,\n"
            "                  Adreno750DeviceId) == 0",
            profile,
        )
        self.assertIn("ExactProprietaryAdreno750RuntimeQuirks(", instance)
        self.assertIn("a750_same_cb_storage_hazard", buffer)
        self.assertIn("(access_mask & vk::AccessFlagBits2::eShaderWrite)", buffer)
        self.assertIn("require_storage_buffer_read_write_barrier && is_storage", rasterizer)
        self.assertIn("vk::AccessFlagBits2::eShaderRead |", rasterizer)
        self.assertIn("vk::AccessFlagBits2::eShaderWrite", rasterizer)

    def test_a750_image_transit_stops_after_terminal_submit_without_changing_other_devices(self):
        image = (ROOT / "funnel-arm/src/video_core/texture_cache/image.cpp").read_text(
            encoding="utf-8"
        )
        transit = image.split("void Image::Transit(", 1)[1].split(
            "void Image::Upload(", 1
        )[0]
        self.assertIn(
            "instance->GetCapabilityProfile().exact_proprietary_adreno750",
            transit,
        )
        self.assertGreaterEqual(transit.count("scheduler->IsDeviceLost()"), 3)
        self.assertIn("scheduler->HasRecordingCommandBuffer()", transit)
        self.assertIn("if (guard_terminal_a750", transit)
        self.assertNotIn("CapabilityQuirk::", transit.split("guard_terminal_a750", 1)[0])

    def test_a750_readonly_depth_access_keeps_gen5_on_current_path(self):
        profile = (ROOT / "funnel-arm/src/video_core/renderer_vulkan/vk_capability_profile.h").read_text(
            encoding="utf-8"
        )
        rasterizer = (ROOT / "funnel-arm/src/video_core/renderer_vulkan/vk_rasterizer.cpp").read_text(
            encoding="utf-8"
        )
        self.assertIn("ReadOnlyDepthStencilAccess", profile)
        self.assertIn("0x44050a31, true, true, true, true) == 0", profile)
        self.assertIn("MobileDriverFamily::MesaTurnip", profile)
        self.assertIn("precise_read_only_access", rasterizer)
        self.assertIn("CapabilityQuirk::ReadOnlyDepthStencilAccess", rasterizer)

    def test_a750_split_depth_stencil_barriers_are_exact_and_feature_gated(self):
        profile = (ROOT / "funnel-arm/src/video_core/renderer_vulkan/vk_capability_profile.h").read_text(
            encoding="utf-8"
        )
        instance = (ROOT / "funnel-arm/src/video_core/renderer_vulkan/vk_instance.cpp").read_text(
            encoding="utf-8"
        )
        image = (ROOT / "funnel-arm/src/video_core/texture_cache/image.cpp").read_text(
            encoding="utf-8"
        )
        self.assertIn("CapabilityQuirk::SplitDepthStencilImageBarriers", profile)
        self.assertIn("0x44050a31, true, true, true, true) == 0", profile)
        self.assertIn("run-sm8650-a750-split-depth-stencil-barriers-ab", instance)
        self.assertIn("vk12_features.separateDepthStencilLayouts", instance)
        self.assertIn("IsSeparateDepthStencilLayoutsSupported()", image)
        self.assertIn("CapabilityQuirk::SplitDepthStencilImageBarriers", image)
        self.assertIn("vk::ImageAspectFlagBits::eDepth", image)
        self.assertIn("vk::ImageAspectFlagBits::eStencil", image)
        self.assertNotIn("Adreno750DeviceId", image)

    def test_a750_barrier_capture_cannot_reach_gen5_or_turnip(self):
        profile = (ROOT / "funnel-arm/src/video_core/renderer_vulkan/vk_capability_profile.h").read_text(
            encoding="utf-8"
        )
        instance = (ROOT / "funnel-arm/src/video_core/renderer_vulkan/vk_instance.cpp").read_text(
            encoding="utf-8"
        )
        self.assertIn("EnableExactProprietaryAdreno750BarrierCapture", profile)
        self.assertIn("QualcommVendorId, 0x44050a31, true", profile)
        self.assertIn("MobileDriverFamily::MesaTurnip, QualcommVendorId, Adreno750DeviceId, true", profile)
        self.assertIn("run-diagnostic-sm8650-a750-vk-barrier-capture", instance)
        self.assertIn("capability_profile.diagnostic_a750_barrier_capture", instance)
        self.assertIn("dispatch = &ExecutorA750CaptureCmdPipelineBarrier2", instance)

    def test_sampler_gc_defers_sampler_destruction_until_gpu_retirement(self):
        presenter = (ROOT / "funnel-arm/src/video_core/renderer_vulkan/vk_presenter.cpp").read_text(
            encoding="utf-8"
        )
        texture_cache = (ROOT / "funnel-arm/src/video_core/texture_cache/texture_cache.cpp").read_text(
            encoding="utf-8"
        )
        prepare = presenter.split("Frame* Presenter::PrepareFrame(", 1)[1].split(
            "Frame* Presenter::PrepareBlankFrame(", 1
        )[0]
        self.assertIn("if (!using_live_drawn_target)", prepare)
        self.assertNotIn("if (!using_live_drawn_target ||", prepare)
        self.assertNotIn("ExecutorSnapshotRegisteredImageUid", prepare)

        gc = texture_cache.split("void TextureCache::GarbageCollectSamplers()", 1)[1].split(
            "void TextureCache::RunGarbageCollector", 1
        )[0]
        move_sampler = gc.index(
            "std::make_unique<Sampler>(std::move(sampler_it.value()))"
        )
        erase_sampler = gc.index("samplers.erase(sampler_it)")
        defer_sampler = gc.index("scheduler.DeferOperation", erase_sampler)
        self.assertLess(move_sampler, erase_sampler)
        self.assertLess(erase_sampler, defer_sampler)
        self.assertIn("sampler.reset();", gc[defer_sampler:])
        self.assertIn("[EXECUTOR_SAMPLER_RETIREMENT]", gc)

    def test_a750_present_source_submit_split_is_exact_marker_gated_and_reacquires_cmdbuf(self):
        profile = (ROOT / "funnel-arm/src/video_core/renderer_vulkan/vk_capability_profile.h").read_text(
            encoding="utf-8"
        )
        instance = (ROOT / "funnel-arm/src/video_core/renderer_vulkan/vk_instance.cpp").read_text(
            encoding="utf-8"
        )
        presenter = (ROOT / "funnel-arm/src/video_core/renderer_vulkan/vk_presenter.cpp").read_text(
            encoding="utf-8"
        )
        field = "diagnostic_a750_present_source_submit_split"
        marker = "run-diagnostic-sm8650-a750-present-source-submit-split"
        self.assertIn(field, profile)
        self.assertIn(marker, instance)
        self.assertIn(
            "capability_profile.exact_proprietary_adreno750 &&\n"
            "        a750_present_source_submit_split_requested",
            instance,
        )
        prepare = presenter.split("Frame* Presenter::PrepareFrame(", 1)[1].split(
            "Frame* Presenter::PrepareBlankFrame(", 1
        )[0]
        acquire = prepare.index("ExecutorAcquirePhysicalDrsPresentationSource")
        split = prepare.index("if (split_a750_present_source)")
        composition = prepare.index("if (direct_mobile_composition)", split)
        self.assertLess(acquire, split)
        self.assertLess(split, composition)
        self.assertIn("draw_scheduler.Flush(source_info);", prepare)
        self.assertIn("cmdbuf = draw_scheduler.CommandBuffer();", prepare[split:composition])
        self.assertIn("frame->ready_tick = draw_scheduler.CurrentTick();", prepare[composition:])
        self.assertIn("info.AddWait(draw_scheduler.GetMasterSemaphore()->Handle()", prepare)
        self.assertNotIn("Adreno750DeviceId", prepare)

    def test_a750_videoout_candidate_uid_guard_is_exact_and_preserves_legacy_queue(self):
        profile = (ROOT / "funnel-arm/src/video_core/renderer_vulkan/vk_capability_profile.h").read_text(
            encoding="utf-8"
        )
        instance = (ROOT / "funnel-arm/src/video_core/renderer_vulkan/vk_instance.cpp").read_text(
            encoding="utf-8"
        )
        texture_header = (ROOT / "funnel-arm/src/video_core/texture_cache/texture_cache.h").read_text(
            encoding="utf-8"
        )
        texture_cache = (ROOT / "funnel-arm/src/video_core/texture_cache/texture_cache.cpp").read_text(
            encoding="utf-8"
        )
        rasterizer = (ROOT / "funnel-arm/src/video_core/renderer_vulkan/vk_rasterizer.cpp").read_text(
            encoding="utf-8"
        )

        field = "diagnostic_a750_videoout_candidate_uid"
        marker = "run-diagnostic-sm8650-a750-videoout-candidate-uid"
        slot0_field = "diagnostic_a750_videoout_slot0_tracking"
        slot0_marker = "run-diagnostic-sm8650-a750-videoout-slot0-tracking"
        self.assertIn(field, profile)
        self.assertIn(f"static_assert(!CapabilityProfile{{}}.{field});", profile)
        self.assertIn(marker, instance)
        self.assertIn(
            "capability_profile.exact_proprietary_adreno750 &&\n"
            "        a750_videoout_candidate_uid_requested",
            instance,
        )
        self.assertIn(slot0_field, profile)
        self.assertIn(f"static_assert(!CapabilityProfile{{}}.{slot0_field});", profile)
        self.assertIn(slot0_marker, instance)
        self.assertIn(
            "capability_profile.exact_proprietary_adreno750 &&\n"
            "        a750_videoout_slot0_tracking_requested",
            instance,
        )



        self.assertIn("std::vector<ImageId> executor_live_drawn_color_candidates;", texture_header)
        self.assertIn("struct ExecutorLiveColorCandidateUid", texture_header)
        self.assertIn("executor_live_drawn_color_candidates_uid", texture_header)
        track = texture_cache.split(
            "void TextureCache::ExecutorTrackLiveDrawnColorTarget", 1
        )[1].split("ImageId TextureCache::ExecutorTakeLiveFullResolutionTarget", 1)[0]
        self.assertLess(track.index("std::unique_lock cache_lock{mutex}"),
                        track.index("std::scoped_lock candidates_lock"))
        self.assertIn("slot_images.is_allocated(image_id)", track)
        self.assertIn("ImageFlagBits::Registered", track)
        self.assertIn("current_uid != image_uid", track)
        self.assertIn("executor_live_drawn_color_candidates.push_back(image_id);", track)
        self.assertIn("const u32 tracking_key = image_index + 1;", track)
        self.assertIn("if (slot0_tracking)", track)

        take = texture_cache.split(
            "ImageId TextureCache::ExecutorTakeLiveFullResolutionTarget", 1
        )[1].split("void TextureCache::ExecutorReplayReadbackColorTarget", 1)[0]
        self.assertIn("std::unique_lock<std::mutex> cache_lock{mutex, std::defer_lock}", take)
        self.assertLess(take.index("cache_lock.lock();"),
                        take.index("std::scoped_lock candidates_lock"))
        self.assertIn("guarded_candidates.swap(executor_live_drawn_color_candidates_uid)", take)
        self.assertIn("candidates.swap(executor_live_drawn_color_candidates)", take)
        self.assertIn("ImageFlagBits::Registered", take)
        self.assertIn("image.image_uid != candidate.image_uid", take)
        self.assertIn("!uid_guard && std::getenv(\"EXECUTOR_TRACE_VIDEOOUT_SOURCE\")", take)

        self.assertEqual(rasterizer.count("ExecutorTrackLiveDrawnColorTarget("), 3)
        self.assertGreaterEqual(rasterizer.count("cb_image_uids[0]"), 2)
        self.assertIn("mrt1_image.image_uid", rasterizer)
        delete = texture_cache.split("void TextureCache::DeleteImage", 1)[1].split(
            "const auto& meta_info", 1
        )[0]
        self.assertIn("std::erase_if(executor_live_drawn_color_candidates_uid", delete)
        self.assertIn("expected_uid = image.image_uid", delete)
        self.assertIn("slot0_tracking ? image_id.index + 1 : image_id.index", delete)
        self.assertNotIn("Adreno750DeviceId", track + take + rasterizer)

    def test_a750_host_pass_journal_is_exact_cached_and_non_behavioral(self):
        scheduler_h = (
            ROOT / "funnel-arm/src/video_core/renderer_vulkan/vk_scheduler.h"
        ).read_text(encoding="utf-8")
        scheduler = (
            ROOT / "funnel-arm/src/video_core/renderer_vulkan/vk_scheduler.cpp"
        ).read_text(encoding="utf-8")
        presenter = (
            ROOT / "funnel-arm/src/video_core/renderer_vulkan/vk_presenter.cpp"
        ).read_text(encoding="utf-8")
        fsr = (
            ROOT
            / "funnel-arm/src/video_core/renderer_vulkan/host_passes/fsr_pass.cpp"
        ).read_text(encoding="utf-8")
        post = (
            ROOT
            / "funnel-arm/src/video_core/renderer_vulkan/host_passes/pp_pass.cpp"
        ).read_text(encoding="utf-8")

        requested = scheduler.split(
            "bool ExecutorVkPersistentJournalRequested()", 1
        )[1].split("bool ExecutorVkJournalRecordEnabled", 1)[0]
        self.assertIn(
            'std::getenv("EXECUTOR_VK_JOURNAL") != nullptr;',
            requested,
        )
        public_gate = scheduler.split(
            "bool ExecutorVkJournalRequested() noexcept", 1
        )[1].split("bool ExecutorVkJournalWouldRecord", 1)[0]
        self.assertIn("return ExecutorVkPersistentJournalRequested();", public_gate)
        self.assertIn("ExecutorVkJournalPresenterStage", scheduler_h)
        self.assertIn("ExecutorVkJournalOp::Presenter", scheduler)
        graph_gate = scheduler.split("bool ExecutorVkIsGraphOperation", 1)[1].split(
            "ExecutorVkGraphSpan ExecutorVkMakeGraphSpan", 1
        )[0]
        self.assertNotIn("Presenter", graph_gate)

        exact_gate = (
            "GetCapabilityProfile().exact_proprietary_adreno750 &&\n"
            "        ExecutorVkJournalRequested()"
        )
        self.assertIn(exact_gate, presenter)
        self.assertIn(exact_gate, fsr)
        self.assertIn(exact_gate, post)
        for stage in (
            "FsrEnterBarrier",
            "FsrEasuDispatch",
            "FsrRcasBarrier",
            "FsrRcasDispatch",
            "FsrOutputBarrier",
        ):
            self.assertIn(f"ExecutorVkJournalPresenterStage::{stage}", fsr)
        for stage in ("PostProcessDraw", "PostProcessOutputBarrier"):
            self.assertIn(f"ExecutorVkJournalPresenterStage::{stage}", post)
        self.assertNotIn("draw_scheduler.Flush", fsr + post)

    def test_a750_fault_timestamp_dump_is_exact_nonblocking_and_prefault_cached(self):
        scheduler = (
            ROOT / "funnel-arm/src/video_core/renderer_vulkan/vk_scheduler.cpp"
        ).read_text(encoding="utf-8")
        scheduler_h = (
            ROOT / "funnel-arm/src/video_core/renderer_vulkan/vk_scheduler.h"
        ).read_text(encoding="utf-8")

        initialize = scheduler.split("void Scheduler::InitializeGpuTimestampRing()", 1)[
            1
        ].split("void Scheduler::CollectGpuTimestampSample()", 1)[0]
        self.assertIn("exact_proprietary_adreno750", initialize)
        self.assertIn("ExecutorVkJournalRequested()", initialize)
        self.assertIn("gpu_fault_journal_enabled", initialize)
        self.assertIn("run-bisect-disable-vk-gpu-timestamps", initialize)
        self.assertIn("run-a750-vk-draw-checkpoints", initialize)
        self.assertIn("GpuTimelineSchedulerKind::GuestDraw", initialize)

        dump = scheduler.split("void Scheduler::DumpGpuTimestampFaultState", 1)[1].split(
            "void Scheduler::BindHostDescriptors", 1
        )[0]
        self.assertIn("exact_proprietary_adreno750", dump)
        self.assertIn("g_gpu_timestamp_scheduler_registry_mutex", dump)
        self.assertIn("scheduler->instance.GetDevice().getQueryPoolResults", dump)
        self.assertIn("fault_query_data_valid", dump)
        self.assertIn("slot.last_query_data_valid", dump)
        self.assertIn("gpu_fault_checkpoint_query_pool", dump)
        self.assertIn("checkpoint_query_data_valid", dump)
        self.assertIn("kind=preEnd", dump)
        self.assertIn("EXECUTOR_GPU_FAULT_DRAW_FRONTIER", dump)
        self.assertNotIn("eWait", dump)
        self.assertNotIn("Check(", dump)

        checkpoint = scheduler.split(
            "void Scheduler::RecordGpuFaultDrawCheckpoint", 1
        )[1].split("void Scheduler::FinishGpuTimestampSample", 1)[0]
        self.assertIn("gpu_fault_draw_checkpoints_enabled", checkpoint)
        self.assertIn("eBottomOfPipe", checkpoint)
        self.assertNotIn("QueueSubmit", checkpoint)
        self.assertNotIn("Wait(", checkpoint)

        rasterizer = (
            ROOT / "funnel-arm/src/video_core/renderer_vulkan/vk_rasterizer.cpp"
        ).read_text(encoding="utf-8")
        self.assertEqual(rasterizer.count("RecordGpuFaultDrawCheckpoint("), 2)

        collect = scheduler.split("void Scheduler::CollectGpuTimestampSample()", 1)[
            1
        ].split("void Scheduler::BeginGpuTimestampSample()", 1)[0]
        self.assertIn("sampled_slot.last_top_available", collect)
        self.assertIn("sampled_slot.last_bottom_available", collect)
        self.assertIn("last_query_data_valid", scheduler_h)

        failure = scheduler.split(
            "if (final_submit_result != vk::Result::eSuccess)", 1
        )[1].split("PublishRetirementTick", 1)[0]
        self.assertLess(
            failure.index("DumpGpuTimestampFaultState"),
            failure.index("DumpDeviceFaultInfo"),
        )


if __name__ == "__main__":
    unittest.main()
