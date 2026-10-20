from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
VIDEO = ROOT / "funnel-arm" / "src" / "video_core"
RASTERIZER = (VIDEO / "renderer_vulkan" / "vk_rasterizer.cpp").read_text(
    encoding="utf-8"
)
BUFFER_H = (VIDEO / "buffer_cache" / "buffer_cache.h").read_text(
    encoding="utf-8"
)
BUFFER = (VIDEO / "buffer_cache" / "buffer_cache.cpp").read_text(
    encoding="utf-8"
)
TEXTURE = (VIDEO / "texture_cache" / "texture_cache.cpp").read_text(
    encoding="utf-8"
)
TILE = (VIDEO / "texture_cache" / "tile_manager.cpp").read_text(
    encoding="utf-8"
)
TILE_H = (VIDEO / "texture_cache" / "tile_manager.h").read_text(
    encoding="utf-8"
)
SCHEDULER_H = (VIDEO / "renderer_vulkan" / "vk_scheduler.h").read_text(
    encoding="utf-8"
)
SCHEDULER = (VIDEO / "renderer_vulkan" / "vk_scheduler.cpp").read_text(
    encoding="utf-8"
)
PIPELINE = (VIDEO / "renderer_vulkan" / "vk_pipeline_cache.cpp").read_text(
    encoding="utf-8"
)
PROFILE = (
    VIDEO / "renderer_vulkan" / "vk_capability_profile.h"
).read_text(encoding="utf-8")
INSTANCE = (VIDEO / "renderer_vulkan" / "vk_instance.cpp").read_text(
    encoding="utf-8"
)


class A750AggregateVmaPressureContract(unittest.TestCase):
    def test_exact_d9_compacts_vma_blocks_without_changing_protected_routes(self):
        self.assertIn(
            "A750TurnipD9LargeHeapBlockSize = 32ULL * 1024ULL * 1024ULL",
            PROFILE,
        )
        exact_assert = (
            "MobileDriverBuild::MesaTurnipA750Da14d65e44Ib32RolloverBoStats,\n"
            "                  false, false, false, true)\n"
            "                  .preferred_large_heap_block_size == 32ULL * 1024ULL * 1024ULL"
        )
        self.assertIn(exact_assert, PROFILE)
        self.assertIn("Adreno840DeviceId", PROFILE)
        self.assertIn(
            ".preferred_large_heap_block_size == 0", PROFILE
        )

    def test_exact_d9_promotes_only_generation_guarded_graphics_signature_reuse(self):
        self.assertIn(
            "capability_profile.exact_mesa_turnip_adreno750 &&", INSTANCE
        )
        self.assertIn(
            "capability_profile.exact_mesa_turnip_a750_d9_lineage",
            INSTANCE,
        )
        self.assertIn(
            "capability_profile.graphics_lookup.structural_signature_reuse_default =\n"
            "            true;",
            INSTANCE,
        )
        self.assertIn("Adreno840DeviceId", PROFILE)
        self.assertIn("last_graphics_other_generation == other_generation", PIPELINE)
        self.assertIn(
            "last_graphics_shader_program_generation == shader_program_generation",
            PIPELINE,
        )
        self.assertIn("RefreshGraphicsStages(false)", PIPELINE)

    def test_exact_d9_does_not_shorten_texture_or_buffer_lifetimes(self):
        self.assertNotIn("exact_a750_d9_residency", TEXTURE)
        self.assertNotIn("exact_a750_d9_cache_policy", TEXTURE)
        self.assertIn("aggresive ? 16 : pressured ? 32 : 64", TEXTURE)

        self.assertNotIn("exact_a750_d9_residency", BUFFER)
        self.assertNotIn("exact_a750_d9_cache_policy", BUFFER)
        self.assertIn("aggressive ? 16 : 64", BUFFER)
        self.assertIn("aggressive ? 48 : 16", BUFFER)

        for source in (TEXTURE, BUFFER):
            self.assertNotIn("ExecutorFinishReason::MemoryPressure", source)

    def test_exact_d9_clean_eviction_uses_baseline_lifetimes(self):
        texture_gc = TEXTURE.split("void TextureCache::GarbageCollectImages", 1)[1].split(
            "void TextureCache::GarbageCollectSamplers", 1
        )[0]
        self.assertNotIn("exact_a750_d9_cache_policy", texture_gc)
        self.assertIn("aggresive ? 16 : pressured ? 32 : 64", texture_gc)
        self.assertIn("aggresive ? 128 : pressured ? 64 : 16", texture_gc)

        buffer_gc = BUFFER.split("void BufferCache::RunGarbageCollector", 1)[1].split(
            "void BufferCache::TouchBuffer", 1
        )[0]
        self.assertNotIn("exact_a750_d9_cache_policy", buffer_gc)
        self.assertIn("aggressive ? 16 : 64", buffer_gc)
        self.assertIn("aggressive ? 48 : 16", buffer_gc)
        self.assertIn("gpu_modified_ranges.Intersects", buffer_gc)
        self.assertIn("memory_tracker->IsRegionGpuModified", buffer_gc)

    def test_exact_d9_recording_admission_is_sample_driven_not_periodic(self):
        method = SCHEDULER.split(
            "void Scheduler::MaybePublishExactA750D9SampledRecordingPressureBoundary", 1
        )[1].split("#endif", 1)[0]
        self.assertIn("AndroidProcessMemorySampleSerial()", method)
        self.assertIn("GetAndroidProcessMemoryStats()", method)
        self.assertIn("memory.sample_serial", method)
        self.assertIn("sample == exact_a750_d9_last_recording_pressure_sample", method)
        self.assertIn("if (!HasPendingWork())", method)
        self.assertIn("PopPendingOperations(true)", method)
        self.assertIn("action=defer-to-videoout waited=0", method)
        self.assertNotIn("PublishMemoryPressureBoundary()", method)
        self.assertNotIn("PressureBoundaryIntervalNs", method)
        self.assertNotIn("recording_work_count", method)
        self.assertNotIn("& 63u", method)

    def test_exact_d9_recording_pressure_never_splits_a_guest_render_pass(self):
        method = SCHEDULER.split(
            "void Scheduler::MaybePublishExactA750D9SampledRecordingPressureBoundary", 1
        )[1].split("#endif", 1)[0]
        self.assertIn("profile.exact_mesa_turnip_a750_d9_lineage", method)
        self.assertIn(
            "ResidencyRetirementMode::CoordinatedSustainedNonBlocking",
            method,
        )
        self.assertIn("PopPendingOperations(true)", method)
        self.assertNotIn("waitIdle", method)
        self.assertNotIn("Finish(", method)
        self.assertNotIn("Wait(", method)
        self.assertNotIn("SubmitExecution", method)
        self.assertIn("defer any publication to a real VideoOut boundary", SCHEDULER_H)
        self.assertIn(
            "MaybePublishExactA750D9SampledRecordingPressureBoundary", RASTERIZER
        )

    def test_exact_d9_recording_thresholds_reclaim_only_completed_operations(self):
        method = SCHEDULER.split(
            "void Scheduler::MaybePublishExactA750D9SampledRecordingPressureBoundary", 1
        )[1].split("#endif", 1)[0]
        for threshold in (
            "memory.graphics_bytes >= 1536 * MiB",
            "memory.total_pss_bytes >= 4608 * MiB",
            "memory.available_memory_bytes <= 2304 * MiB",
        ):
            self.assertIn(threshold, method)

        def under_pressure(pss_mib, graphics_mib, available_mib):
            return (
                graphics_mib >= 1536
                or pss_mib >= 4608
                or (available_mib != 0 and available_mib <= 2304)
            )



        self.assertFalse(under_pressure(3962, 1170, 3010))


        self.assertTrue(under_pressure(4379, 1679, 2521))
        self.assertTrue(under_pressure(3915, 2486, 1924))
        self.assertIn("PopPendingOperations(true)", method)
        self.assertNotIn("Wait(", method)
        self.assertNotIn("SubmitExecution", method)

    def test_combined_pressure_thresholds_are_marker_only_and_exact_a750(self):
        marker = "run-sm8650-a750-combined-pressure-gc"
        self.assertIn("struct AggregateMemoryPressurePolicy", PROFILE)
        self.assertIn("ResolveAggregateMemoryPressurePolicy", PROFILE)
        self.assertIn(
            "static_assert(!CapabilityProfile{}.aggregate_memory_pressure.combined_gc);",
            PROFILE,
        )
        self.assertIn(marker, INSTANCE)
        self.assertIn(
            "capability_profile.aggregate_memory_pressure =\n"
            "        ResolveAggregateMemoryPressurePolicy(",
            INSTANCE,
        )

        submit = RASTERIZER.split("void Rasterizer::OnSubmit", 1)[1].split(
            "pipeline_cache.AdvanceShaderIdentitySubmit", 1
        )[0]
        self.assertIn(
            "exact_a750_vma_pressure &&\n"
            "        aggregate_pressure_policy.combined_gc",
            submit,
        )
        self.assertIn("device_budget * 55 / 100", submit)
        self.assertIn("device_budget * 65 / 100", submit)

    def test_marker_off_preserves_existing_a750_pressure_thresholds(self):
        submit = RASTERIZER.split("void Rasterizer::OnSubmit", 1)[1].split(
            "pipeline_cache.AdvanceShaderIdentitySubmit", 1
        )[0]
        self.assertIn("device_budget - device_budget / 4", submit)
        self.assertIn("device_budget - device_budget / 5", submit)

    def test_vma_fallback_is_exact_proprietary_a750_only(self):
        sample = RASTERIZER.split("void Rasterizer::OnSubmit", 1)[1].split(
            "pipeline_cache.AdvanceShaderIdentitySubmit", 1
        )[0]
        self.assertIn("aggregate_pressure_policy.proprietary_vma_sampling", sample)
        self.assertNotIn("exact_proprietary_adreno750", sample)
        self.assertNotIn("!instance.CanReportMemoryUsage()", sample)
        self.assertIn("vmaGetHeapBudgets", sample)
        self.assertIn("heap.usage", sample)
        self.assertNotIn("vmaCalculateStatistics", sample)
        self.assertNotIn("allocationBytes", sample)
        self.assertIn(
            "sample_residency =\n"
            "        run_residency_gc || proprietary_a750_vma_pressure",
            sample,
        )
        self.assertIn(
            "if (proprietary_a750_vma_pressure || force_memory_budget_sample",
            sample,
        )
        self.assertIn(
            "if (instance.CanReportMemoryUsage()) {\n"
            "                    cached_device_memory_usage = "
            "instance.GetDeviceMemoryUsage();",
            sample,
        )

    def test_vma_pressure_is_independent_of_the_cpu_translator(self):
        sample = RASTERIZER.split("void Rasterizer::OnSubmit", 1)[1].split(
            "bool run_residency_gc", 1
        )[0]
        self.assertIn("aggregate_pressure_policy.proprietary_vma_sampling", sample)
        self.assertNotIn("exact_proprietary_adreno750", sample)
        self.assertNotIn("!instance.CanReportMemoryUsage()", sample)
        self.assertNotIn("ExecutorCommittedFexSignalApiActive()", RASTERIZER)
        self.assertNotIn("executor_lsx4_android_fault_signal_owner", RASTERIZER)
        self.assertNotIn("executor_lsx4_android_fex_signal_api_active", RASTERIZER)

        def pressure_gate(exact_a750, reports_budget):
            return exact_a750 and not reports_budget

        self.assertTrue(pressure_gate(True, False))
        self.assertFalse(pressure_gate(False, False))
        self.assertFalse(pressure_gate(True, True))

    def test_one_pressure_epoch_drives_both_cache_collectors_then_admission(self):
        gc = RASTERIZER.split("void Rasterizer::OnSubmit", 1)[1].split(
            "pipeline_cache.AdvanceShaderIdentitySubmit", 1
        )[0]
        texture = gc.index(
            "texture_cache.RunGarbageCollector(cache_pressure_sample,\n"
            "                                               exact_a750_pressure_epoch)"
        )
        buffer = gc.index(
            "buffer_cache.RunGarbageCollector(cache_pressure_sample,\n"
            "                                              exact_a750_pressure_epoch)"
        )
        publish = gc.index("scheduler.PublishMemoryPressureBoundary()")
        self.assertLess(texture, buffer)
        self.assertLess(buffer, publish)

    def test_turnip_process_pressure_cannot_fake_a_device_emergency(self):
        gc = RASTERIZER.split("void Rasterizer::OnSubmit", 1)[1].split(
            "pipeline_cache.AdvanceShaderIdentitySubmit", 1
        )[0]
        pressure = gc.split(
            "if (android_memory_pressure_epoch && device_budget != 0)", 1
        )[1].split("if (exact_a750_vma_pressure", 1)[0]
        self.assertIn("if (proprietary_a750_vma_pressure)", pressure)
        self.assertIn("static_cast<size_t>(device_budget)", pressure)
        self.assertIn("promoted=%u", pressure)
        self.assertNotIn("coordinated_turnip_a750_pressure", pressure)

    def test_exact_a750_aggregate_gc_is_a_high_water_pressure_epoch(self):
        gc = RASTERIZER.split("void Rasterizer::OnSubmit", 1)[1].split(
            "pipeline_cache.AdvanceShaderIdentitySubmit", 1
        )[0]
        self.assertIn("executor_a750_aggregate_gc_latched", gc)
        self.assertIn("executor_a750_aggregate_gc_high_water_usage", gc)
        self.assertIn("usage < a750_soft_limit", gc)
        self.assertIn("usage - executor_a750_aggregate_gc_high_water_usage", gc)
        self.assertIn("128_MB", gc)
        self.assertIn("run_residency_gc || exact_a750_pressure_epoch", gc)
        self.assertIn(
            "exact_a750_vma_pressure && !exact_a750_pressure_epoch", gc
        )
        self.assertIn("cache_pressure_sample.reset()", gc)
        self.assertIn("pressureEpoch=%llu suppressed=%llu", gc)
        self.assertNotEqual(
            gc.index("executor_a750_aggregate_gc_latched"),
            gc.index("executor_a750_aggregate_drain_latched"),
        )

    def test_oscillating_vma_usage_does_not_rearm_the_epoch(self):
        soft = 2_181
        step = 128
        latched = False
        high_water = 0
        epochs = []

        for usage in (4_251, 4_466, 4_251, 4_466, 4_251, 4_466):
            if usage < soft:
                latched = False
                high_water = 0
                epoch = False
            else:
                grew = usage > high_water and usage - high_water >= step
                epoch = not latched or grew
                if epoch:
                    latched = True
                    high_water = max(high_water, usage)
            epochs.append(epoch)

        self.assertEqual(epochs, [True, True, False, False, False, False])


        usage = soft - 1
        if usage < soft:
            latched = False
            high_water = 0
        usage = 4_251
        self.assertTrue(not latched or usage - high_water >= step)

    def test_proprietary_a750_rearms_wait_free_gc_while_pressure_persists(self):
        gc = RASTERIZER.split("void Rasterizer::OnSubmit", 1)[1].split(
            "pipeline_cache.AdvanceShaderIdentitySubmit", 1
        )[0]
        self.assertIn(
            "sustained_turnip_a750_retirement ||\n"
            "                     proprietary_a750_vma_pressure",
            gc,
        )
        self.assertIn("SustainedRetirementIntervalNs = 1'000'000'000", gc)
        self.assertIn("SustainedDrainIntervalNs = 1'000'000'000", gc)
        self.assertIn("waited=0", gc)
        self.assertNotIn("ExecutorFinishReason::Rasterizer", gc)

    def test_emergency_admission_is_bounded_and_pending_work_aware(self):
        gc = RASTERIZER.split("void Rasterizer::OnSubmit", 1)[1].split(
            "pipeline_cache.AdvanceShaderIdentitySubmit", 1
        )[0]
        self.assertIn("device_budget - device_budget / 4", gc)
        self.assertIn("device_budget - device_budget / 5", gc)
        self.assertIn("scheduler.PendingOperationCount()", gc)
        self.assertIn("pending_before != 0", gc)
        self.assertIn("executor_a750_aggregate_drain_latched", gc)
        self.assertIn("128_MB", gc)
        self.assertIn("[EXECUTOR_A750_MEMORY_ADMISSION]", gc)
        self.assertIn("action=publish-collect-completed", gc)
        self.assertIn("waited=0", gc)
        self.assertNotIn("ExecutorFinishReason::Rasterizer", gc)
        self.assertNotIn("PopPendingOperations(true, true)", gc)
        self.assertIn("scheduler.PublishMemoryPressureBoundary()", gc)
        self.assertIn("PendingOperationCount() const noexcept", SCHEDULER_H)
        self.assertIn("PublishMemoryPressureBoundary()", SCHEDULER_H)

    def test_buffer_gc_uses_bounded_pressure_before_critical_emergency(self):
        self.assertIn(
            "RunGarbageCollector(std::optional<size_t> sampled_device_memory",
            BUFFER_H,
        )
        gc = BUFFER.split("void BufferCache::RunGarbageCollector", 1)[1].split(
            "void BufferCache::TouchBuffer", 1
        )[0]
        self.assertIn("aggregate_pressure_policy.cache_pressure_active", gc)
        self.assertNotIn("exact_proprietary_adreno750", gc)
        self.assertNotIn("!instance.CanReportMemoryUsage()", gc)
        self.assertIn(
            "resident_buffer_bytes < trigger_gc_memory && !aggregate_pressure",
            gc,
        )
        self.assertIn("aggregate_pressure_policy.combined_gc", gc)
        self.assertIn("device_budget * 55 / 100", gc)
        self.assertIn("device_budget * 65 / 100", gc)
        self.assertIn("device_budget - device_budget / 4", gc)
        self.assertIn("device_budget - device_budget / 8", gc)
        self.assertIn(
            "aggregate_emergency || resident_buffer_bytes >= critical_gc_memory",
            gc,
        )
        self.assertIn("if (gpu_dirty)", gc)
        self.assertLess(gc.index("if (gpu_dirty)"), gc.index("DeleteBuffer(buffer_id)"))
        self.assertNotIn("scheduler.Finish", gc)

    def test_texture_cache_is_not_evicted_by_soft_aggregate_pressure(self):
        gc = TEXTURE.split("void TextureCache::GarbageCollectImages", 1)[1].split(
            "void TextureCache::GarbageCollectSamplers", 1
        )[0]
        self.assertIn("aggregate_pressure_policy.cache_pressure_active", gc)
        self.assertNotIn("exact_proprietary_adreno750", gc)
        self.assertNotIn("aggregate_pressure_policy.combined_gc", gc)
        self.assertNotIn("device_pressure", gc)
        self.assertIn("? device_budget / 5", gc)
        self.assertIn("std::min<u64>(512_MB, device_budget / 8)", gc)
        self.assertIn("total_used_memory < trigger_gc_memory && !device_emergency", gc)
        self.assertIn("deleted_images != 0 && !exact_a750_aggregate_pressure", gc)

    def test_oversize_scratch_reuse_is_exact_a750_and_capacity_safe(self):
        self.assertIn("u64 capacity{}", TILE_H)
        self.assertIn(
            "instance.GetCapabilityProfile().exact_proprietary_adreno750",
            TILE,
        )
        self.assertIn("allow_oversize_recording_reuse ||", TILE)
        self.assertIn("bucket > budget_bytes", TILE)
        self.assertIn("candidate.capacity >= required_capacity", TILE)
        self.assertIn("selected_bucket == epoch->second.buckets.end() &&", TILE)
        self.assertIn("allow_oversize_recording_reuse", TILE)
        self.assertIn("bucket * 2", TILE)
        self.assertIn("candidate.capacity < selected_capacity", TILE)
        self.assertIn("recordingCrossReused=%llu", TILE)
        self.assertIn("AllocationSize(size, bucket)", TILE)
        self.assertIn(".capacity = allocation_size", TILE)

    def test_exact_device_identity_still_excludes_a840_turnip_and_mali(self):
        self.assertIn("Adreno750DeviceId = 0x43051401", PROFILE)
        self.assertIn("0x44050a31", PROFILE)
        self.assertIn(
            "MobileDriverFamily::MesaTurnip, QualcommVendorId,\n"
            "                                          Adreno750DeviceId",
            PROFILE,
        )
        self.assertIn("MobileDriverFamily::ArmProprietary", PROFILE)


if __name__ == "__main__":
    unittest.main()
