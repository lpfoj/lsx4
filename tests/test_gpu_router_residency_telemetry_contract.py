import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
PIPELINE_CACHE = (
    ROOT
    / "funnel-arm"
    / "src"
    / "video_core"
    / "renderer_vulkan"
    / "vk_pipeline_cache.cpp"
).read_text(encoding="utf-8")
PIPELINE_HEADER = (
    ROOT
    / "funnel-arm"
    / "src"
    / "video_core"
    / "renderer_vulkan"
    / "vk_pipeline_cache.h"
).read_text(encoding="utf-8")
RUNTIME = (ROOT / "src" / "executor" / "native_runtime_api.cpp").read_text(
    encoding="utf-8"
)
JNI = (ROOT / "android-app" / "app" / "src" / "main" / "cpp" / "runtime_bridge.cpp").read_text(
    encoding="utf-8"
)
JAVA = (ROOT / "android-app" / "app" / "src" / "main" / "java" / "app" / "lsx4" / "android" / "RuntimeBridge.java").read_text(
    encoding="utf-8"
)
RECEIVER = (ROOT / "android-app" / "app" / "src" / "main" / "java" / "app" / "lsx4" / "android" / "ProofMetricsReceiver.java").read_text(
    encoding="utf-8"
)
CAPABILITY_PROFILE = (
    ROOT
    / "funnel-arm"
    / "src"
    / "video_core"
    / "renderer_vulkan"
    / "vk_capability_profile.h"
).read_text(encoding="utf-8")
VK_INSTANCE = (
    ROOT
    / "funnel-arm"
    / "src"
    / "video_core"
    / "renderer_vulkan"
    / "vk_instance.cpp"
).read_text(encoding="utf-8")
VK_INSTANCE_HEADER = (
    ROOT
    / "funnel-arm"
    / "src"
    / "video_core"
    / "renderer_vulkan"
    / "vk_instance.h"
).read_text(encoding="utf-8")
BUFFER_IMPL = (
    ROOT / "funnel-arm" / "src" / "video_core" / "buffer_cache" / "buffer.cpp"
).read_text(encoding="utf-8")
BUFFER_CACHE_IMPL = (
    ROOT / "funnel-arm" / "src" / "video_core" / "buffer_cache" / "buffer_cache.cpp"
).read_text(encoding="utf-8")
SCHEDULER = (
    ROOT
    / "funnel-arm"
    / "src"
    / "video_core"
    / "renderer_vulkan"
    / "vk_scheduler.cpp"
).read_text(encoding="utf-8")


class GpuRouterResidencyTelemetryContractTests(unittest.TestCase):
    def test_a750_memory_policy_is_exact_and_neighbors_keep_default(self):
        self.assertIn("struct MemoryPolicy", CAPABILITY_PROFILE)
        self.assertIn("ResolveMobileMemoryPolicy", CAPABILITY_PROFILE)
        self.assertIn("A750QualcommLargeHeapBlockSize", CAPABILITY_PROFILE)
        self.assertIn("MobileDriverFamily::MesaTurnip", CAPABILITY_PROFILE)
        self.assertIn("Adreno840DeviceId", CAPABILITY_PROFILE)
        self.assertIn("capability_profile.memory_policy", VK_INSTANCE)
        self.assertIn("run-disable-sm8650-a750-vma-block64", VK_INSTANCE)
        self.assertIn("run-sm8650-a750-turnip-vma-block64-ab", VK_INSTANCE)
        self.assertIn("exact_mesa_turnip_adreno750", VK_INSTANCE)
        self.assertIn("preferredLargeHeapBlockSize = effective_block_size", VK_INSTANCE)
        self.assertIn("MemoryBudgetAccounting", CAPABILITY_PROFILE)
        self.assertIn("ReportedUsageWithInitialBaseline", CAPABILITY_PROFILE)
        self.assertIn("run-sm8650-a750-turnip-budget-baseline-ab", VK_INSTANCE)
        self.assertIn("memory_pressure_usage_limit", VK_INSTANCE)
        self.assertIn("reported_device_memory_budget", VK_INSTANCE)
        self.assertIn("device_initial_memory_usage", VK_INSTANCE)

    def test_bda_sparse_capability_is_measured_from_actual_device(self):
        self.assertIn("IsSparseBufferResidencySupported", VK_INSTANCE_HEADER)
        self.assertIn("graphics_queue_supports_sparse", VK_INSTANCE_HEADER)
        self.assertIn("features.sparseBinding", VK_INSTANCE_HEADER)
        self.assertIn("features.sparseResidencyBuffer", VK_INSTANCE_HEADER)
        self.assertIn("residencyNonResidentStrict", VK_INSTANCE_HEADER)
        self.assertIn("[EXECUTOR_VK_BDA_CAPS]", VK_INSTANCE)
        self.assertIn("maxStorageBufferRangeMiB", VK_INSTANCE)
        self.assertIn("sparseCandidateEligible", VK_INSTANCE)
        self.assertIn("ResolveBdaPageTablePolicy", CAPABILITY_PROFILE)
        self.assertIn("SegmentedSparse", CAPABILITY_PROFILE)
        self.assertIn("FlatSparse", CAPABILITY_PROFILE)
        self.assertIn("run-sm8650-a750-turnip-bda-flat-sparse-ab", VK_INSTANCE)
        self.assertIn("descriptorSegments=%u", VK_INSTANCE)
        self.assertIn(".sparseBinding =", VK_INSTANCE)
        self.assertIn(".sparseResidencyBuffer =", VK_INSTANCE)

    def test_residency_diagnostic_is_marker_gated_to_exact_turnip_d9(self):
        helper = CAPABILITY_PROFILE.index(
            "constexpr bool EnableExactA750TurnipD9ResidencyTelemetry"
        )
        helper_window = CAPABILITY_PROFILE[helper : helper + 520]
        for token in (
            "requested",
            "GpuCompatibilityRoute::Adreno750MesaTurnip",
            "IsMesaTurnipA750D9LineageBuild",
        ):
            self.assertIn(token, helper_window)

        assignment = VK_INSTANCE.index(
            "capability_profile.diagnostic_a750_residency_telemetry ="
        )
        assignment_window = VK_INSTANCE[assignment : assignment + 420]
        self.assertIn(
            "EnableExactA750TurnipD9ResidencyTelemetry", assignment_window
        )
        self.assertIn("a750_residency_telemetry_requested", assignment_window)
        self.assertNotIn("exact_proprietary_adreno750", assignment_window)

        static_tests = CAPABILITY_PROFILE[
            CAPABILITY_PROFILE.index(
                "static_assert(EnableExactA750TurnipD9ResidencyTelemetry"
            ) :
        ]
        for negative_route in (
            "GpuCompatibilityRoute::Adreno840MesaTurnip",
            "GpuCompatibilityRoute::Adreno840QualcommProprietary",
            "GpuCompatibilityRoute::MaliArmProprietary",
            "GpuCompatibilityRoute::Adreno750QualcommProprietary",
            "GpuCompatibilityRoute::PortableDefault",
        ):
            self.assertIn(negative_route, static_tests)
        self.assertIn(
            "MobileDriverBuild::MesaTurnipA750Da14d65e44Ib32Rollover, true",
            static_tests,
        )
        self.assertIn("MobileDriverBuild::Unknown, true", static_tests)
        self.assertIn(
            "MobileDriverBuild::MesaTurnipA750Da14d65e44Ib32RolloverBoStats, false",
            static_tests,
        )
        self.assertIn(
            "MobileDriverBuild::MesaTurnipA750D19FormattedNoLog, true",
            static_tests,
        )

    def test_existing_checkpoint_reports_live_pipeline_residency_axes(self):
        checkpoint = PIPELINE_CACHE.index("[EXECUTOR_GP_CACHE_TELEMETRY]")
        window = PIPELINE_CACHE[checkpoint : checkpoint + 3200]
        for token in (
            "programEntries=%zu",
            "graphicsPipelineEntries=%zu",
            "computePipelineEntries=%zu",
            "newPipelines=%u",
            "shaderIdentityEntries=%zu",
            "srtObjects=%zu",
            "srtRetainedBytes=%zu",
            "program_cache.size()",
            "graphics_pipelines.size()",
            "compute_pipelines.size()",
            "shader_identity_cache.size()",
            "srt_snapshot_object_count",
            "srt_snapshot_retained_bytes",
        ):
            self.assertIn(token, window)

    def test_sparse_bda_residency_is_demand_bound_without_queue_idle(self):
        for token in (
            "[EXECUTOR_BDA_SPARSE_CREATE]",
            "[EXECUTOR_BDA_SPARSE_BIND]",
            "allocateMemory(allocation_info)",
            "freeMemory(staged.physical.memory)",
            "BindSparseBufferMemory",
            "PreferredPhysicalBlockSize",
        ):
            self.assertIn(token, BUFFER_IMPL)
        upload = BUFFER_CACHE_IMPL.index("void BufferCache::WriteDataBuffer")
        upload_window = BUFFER_CACHE_IMPL[upload : upload + 1800]
        self.assertLess(
            upload_window.index("EnsureResident"),
            upload_window.index("QueueBufferUploadCopies"),
        )
        sparse_bind = SCHEDULER.index("Scheduler::BindSparseBufferMemory")
        sparse_window = SCHEDULER[sparse_bind : sparse_bind + 1200]
        self.assertIn("bindSparse", sparse_window)
        self.assertNotIn("Finish(", sparse_window)
        self.assertNotIn("waitIdle", sparse_window)
        self.assertNotIn("CUSA", sparse_window)

    def test_telemetry_does_not_add_a_new_per_lookup_container_walk(self):
        checkpoint = PIPELINE_CACHE.index("[EXECUTOR_GP_CACHE_TELEMETRY]")
        window = PIPELINE_CACHE[checkpoint : checkpoint + 3200]
        self.assertNotIn("for (", window)
        self.assertNotIn("std::accumulate", window)

    def test_proof_snapshot_reads_a_lock_free_publication_not_live_maps(self):
        self.assertIn("PipelineResidencyTelemetrySnapshot", PIPELINE_HEADER)
        self.assertIn("GetPipelineResidencyTelemetrySnapshot", PIPELINE_HEADER)
        self.assertIn("MaybePublishResidencyTelemetry", PIPELINE_CACHE)
        self.assertIn("renderer-published-lock-free-snapshot", RUNTIME)
        self.assertIn("executor_lsx4_runtime_gpu_residency_metrics", RUNTIME)
        self.assertIn("g_gpu_residency_metrics", JNI)
        self.assertIn("gpuResidencyMetrics", JAVA)
        self.assertIn('payload.put("gpuResidency"', RECEIVER)
        runtime_window = RUNTIME[
            RUNTIME.index("executor_lsx4_runtime_gpu_residency_metrics") :
        ]
        self.assertNotIn("program_cache", runtime_window)
        self.assertNotIn("graphics_pipelines", runtime_window)

    def test_residency_snapshot_contains_pipeline_and_memory_axes(self):
        for token in (
            '"programEntries"',
            '"graphicsPipelineEntries"',
            '"computePipelineEntries"',
            '"newPipelines"',
            '"shaderIdentityEntries"',
            '"srtObjects"',
            '"srtRetainedBytes"',
            '"descriptorTopology"',
            '"descriptorLayouts"',
            '"pushDescriptorLayouts"',
            '"legacyDescriptorLayouts"',
            '"maxDescriptorBindings"',
            '"maxPushDescriptors"',
            '"maxUpdateAfterBindDescriptors"',
            '"imageTopology"',
            '"imageCreations"',
            '"mutableImageCreations"',
            '"boundedFormatListImageCreations"',
            '"boundedFormatListEntries"',
            '"maxBoundedFormatListEntries"',
            '"reinterpretedImageViewCreations"',
            '"storageImageViewCreations"',
            '"textureGuestBytes"',
            '"textureImageEntries"',
            '"textureImageViewEntries"',
            '"textureSamplerEntries"',
            '"bufferResidentBytes"',
            '"bufferUtilityBytes"',
            '"bufferEntries"',
            '"observedSamples"',
            '"usedBytes"',
            '"budgetBytes"',
            '"residencyGcPasses"',
            '"residencyGcEvictions"',
            '"utilityRings"',
            '"capacityBytes"',
            '"requestedBytes"',
            '"maxRequestBytes"',
            '"wrapCount"',
            '"forcedWaitCount"',
            '"failedRequestCount"',
            '"uploadReservation"',
            '"fullSpanCandidateBytes"',
            '"dirtyBytes"',
            '"avoidedBytes"',
            '"measurementCount"',
            '"retryCount"',
        ):
            self.assertIn(token, RUNTIME)

    def test_utility_ring_telemetry_is_o1_and_separates_capacity_from_pressure(self):
        buffer_header = (
            ROOT / "funnel-arm" / "src" / "video_core" / "buffer_cache" / "buffer.h"
        ).read_text(encoding="utf-8")
        buffer_cache_header = (
            ROOT
            / "funnel-arm"
            / "src"
            / "video_core"
            / "buffer_cache"
            / "buffer_cache.h"
        ).read_text(encoding="utf-8")
        pipeline_header = (
            ROOT
            / "funnel-arm"
            / "src"
            / "video_core"
            / "renderer_vulkan"
            / "vk_pipeline_cache.h"
        ).read_text(encoding="utf-8")
        rasterizer = (
            ROOT
            / "funnel-arm"
            / "src"
            / "video_core"
            / "renderer_vulkan"
            / "vk_rasterizer.cpp"
        ).read_text(encoding="utf-8")
        for token in (
            "UsageTelemetrySnapshot",
            "requested_bytes",
            "max_request_bytes",
            "max_mapped_end",
            "wrap_count",
            "forced_wait_count",
            "failed_request_count",
        ):
            self.assertIn(token, buffer_header)
        self.assertIn("PublishRendererUtilityRingTelemetry", rasterizer)
        self.assertIn("UploadReservationTelemetrySnapshot", pipeline_header)
        self.assertIn("upload_reservation_avoided_bytes", buffer_cache_header)
        publish = rasterizer.index("PublishRendererUtilityRingTelemetry")
        window = rasterizer[publish - 1800 : publish + 700]
        self.assertNotIn("for (", window)
        self.assertNotIn("std::accumulate", window)

    def test_cache_classes_are_published_from_o1_counters(self):
        rasterizer = (
            ROOT
            / "funnel-arm"
            / "src"
            / "video_core"
            / "renderer_vulkan"
            / "vk_rasterizer.cpp"
        ).read_text(encoding="utf-8")
        buffer_header = (
            ROOT
            / "funnel-arm"
            / "src"
            / "video_core"
            / "buffer_cache"
            / "buffer_cache.h"
        ).read_text(encoding="utf-8")
        texture_header = (
            ROOT
            / "funnel-arm"
            / "src"
            / "video_core"
            / "texture_cache"
            / "texture_cache.h"
        ).read_text(encoding="utf-8")
        self.assertIn("PublishRendererCacheResidencyTelemetry", rasterizer)
        self.assertIn("resident_buffer_bytes", buffer_header)
        self.assertIn("total_used_memory", texture_header)
        publish = rasterizer.index("PublishRendererCacheResidencyTelemetry")
        window = rasterizer[publish - 700 : publish + 900]
        self.assertNotIn("for (", window)
        self.assertNotIn("std::accumulate", window)


if __name__ == "__main__":
    unittest.main()
