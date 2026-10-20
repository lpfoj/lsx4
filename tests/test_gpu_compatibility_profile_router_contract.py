import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
PROFILE = (
    ROOT
    / "funnel-arm"
    / "src"
    / "video_core"
    / "renderer_vulkan"
    / "vk_capability_profile.h"
).read_text(encoding="utf-8")
INSTANCE = (
    ROOT
    / "funnel-arm"
    / "src"
    / "video_core"
    / "renderer_vulkan"
    / "vk_instance.cpp"
).read_text(encoding="utf-8")
RASTERIZER = (
    ROOT
    / "funnel-arm"
    / "src"
    / "video_core"
    / "renderer_vulkan"
    / "vk_rasterizer.cpp"
).read_text(encoding="utf-8")
LIVERPOOL = (
    ROOT / "funnel-arm" / "src" / "video_core" / "amdgpu" / "liverpool.cpp"
).read_text(encoding="utf-8")
TEXTURE_CACHE = (
    ROOT / "funnel-arm" / "src" / "video_core" / "texture_cache" / "texture_cache.cpp"
).read_text(encoding="utf-8")
BUFFER_CACHE = (
    ROOT / "funnel-arm" / "src" / "video_core" / "buffer_cache" / "buffer_cache.cpp"
).read_text(encoding="utf-8")
PIPELINE_SERIALIZATION = (
    ROOT
    / "funnel-arm"
    / "src"
    / "video_core"
    / "renderer_vulkan"
    / "vk_pipeline_serialization.cpp"
).read_text(encoding="utf-8")
PIPELINE_COMMON = (
    ROOT
    / "funnel-arm"
    / "src"
    / "video_core"
    / "renderer_vulkan"
    / "vk_pipeline_common.cpp"
).read_text(encoding="utf-8")
PIPELINE_CACHE = (
    ROOT
    / "funnel-arm"
    / "src"
    / "video_core"
    / "renderer_vulkan"
    / "vk_pipeline_cache.cpp"
).read_text(encoding="utf-8")
DRIVER_OPTIMIZATION = (
    ROOT / "funnel-arm" / "src" / "video_core" / "vulkan_driver_optimization.cpp"
).read_text(encoding="utf-8")
IMGUI_CORE = (
    ROOT / "funnel-arm" / "src" / "imgui" / "renderer" / "imgui_core.cpp"
).read_text(encoding="utf-8")
IMGUI_VULKAN = (
    ROOT / "funnel-arm" / "src" / "imgui" / "renderer" / "imgui_impl_vulkan.cpp"
).read_text(encoding="utf-8")
DRIVER_MANAGER = (
    ROOT
    / "android-app"
    / "app"
    / "src"
    / "main"
    / "java"
    / "app"
    / "lsx4"
    / "android"
    / "VulkanDriverManager.java"
).read_text(encoding="utf-8")


class GpuCompatibilityProfileRouterContractTests(unittest.TestCase):
    def test_hardware_and_icd_are_independent_axes(self):
        for token in (
            "enum class MobileGpuModel",
            "Adreno750",
            "Adreno840",
            "enum class MobileGpuGeneration",
            "MobileGpuGeneration mobile_gpu_generation",
            "MobileDriverFamily reported_mobile_driver_family",
            "MobileDriverFamily mobile_driver_family",
            "MobileGpuModel mobile_gpu_model",
            "GpuCompatibilityRoute gpu_compatibility_route",
            "MobileDriverBuild mobile_driver_build",
            "u32 driver_version",
            "exact_proprietary_adreno750",
            "exact_proprietary_adreno840",
            "exact_mesa_turnip_adreno750",
            "exact_mesa_turnip_adreno840",
        ):
            self.assertIn(token, PROFILE)

    def test_canonical_route_is_actual_icd_by_gpu_and_unknown_is_portable(self):
        for token in (
            "enum class GpuCompatibilityRoute",
            "Adreno750QualcommProprietary",
            "Adreno840QualcommProprietary",
            "Adreno750MesaTurnip",
            "Adreno840MesaTurnip",
            "MaliArmProprietary",
            "ResolveGpuCompatibilityRoute",
            "MobileDriverFamily::Unknown, QualcommVendorId",
            "GpuCompatibilityRoute::PortableDefault",
        ):
            self.assertIn(token, PROFILE)
        self.assertIn(
            "capability_profile.gpu_compatibility_route = ResolveGpuCompatibilityRoute(",
            INSTANCE,
        )
        self.assertIn(
            "route=%s gpu=%s generation=%s "
            '"\n        "driver=%s reportedDriver=%s',
            INSTANCE,
        )
        self.assertNotIn("TitleID", PROFILE)

    def test_generation_layer_is_exact_data_only_and_never_guesses_unknown_adreno(self):
        for token in (
            "ResolveMobileGpuGeneration",
            "MobileGpuGeneration::Adreno7xxGen3",
            "MobileGpuGeneration::Adreno8xxGen2",
            "MobileGpuModel::AdrenoOther) ==\n              MobileGpuGeneration::Unknown",
            "ResolveMobileGpuGeneration(capability_profile.mobile_gpu_model)",
            "MobileGpuGenerationName(",
        ):
            self.assertIn(token, PROFILE + INSTANCE)

    def test_android_driver_admission_is_exact_hardware_before_vulkan_routing(self):
        for token in (
            "isBundledTurnipSupportedHardware",
            "ADRENO_840_VULKAN_DEVICE_ID",
            'VULKAN_HARDWARE_KEY_FIELD = "vulkanHardwareKey"',
            "Selected Vulkan driver is verified for different hardware",
            "Build.FINGERPRINT.equals",
            "currentHardware.matches(verifiedHardware)",
        ):
            self.assertIn(token, DRIVER_MANAGER)
        bundled_start = DRIVER_MANAGER.index(
            "static boolean isBundledTurnipSupportedHardware"
        )
        bundled = DRIVER_MANAGER[bundled_start : bundled_start + 420]
        self.assertIn("ADRENO_840_VULKAN_DEVICE_ID", bundled)
        self.assertIn("ADRENO_750_VULKAN_DEVICE_ID", bundled)
        self.assertIn("QUALCOMM_VULKAN_VENDOR_ID", bundled)

    def test_pipeline_warmup_consumes_pre_resolved_typed_policy(self):
        warmup_start = PIPELINE_SERIALIZATION.index(
            "const PipelineWarmupPolicy warmup_policy"
        )
        warmup = PIPELINE_SERIALIZATION[warmup_start : warmup_start + 560]
        self.assertIn("GetCapabilityProfile().pipeline_warmup", warmup)
        self.assertIn("warmup_policy.maximum_pipelines", warmup)
        self.assertIn("warmup_policy.time_budget_ms", warmup)
        self.assertNotIn("MobileDriverFamily", warmup)
        self.assertNotIn("GetDriverID()", warmup)
        self.assertIn("warmup_policy.allow_background_warmup", PIPELINE_SERIALIZATION)
        for token in (
            "struct PipelineWarmupPolicy",
            "ResolvePipelineWarmupPolicy",
            ".maximum_pipelines = 32",
            ".time_budget_ms = 1250",
            ".allow_background_warmup = false",
            "PipelineWarmupPolicy pipeline_warmup",
        ):
            self.assertIn(token, PROFILE)

    def test_imgui_upload_experiment_consumes_pre_resolved_typed_policy(self):
        self.assertIn(
            "GetCapabilityProfile().imgui_upload.allow_persistent_mapping_ab",
            IMGUI_CORE,
        )
        self.assertIn("info.allow_persistent_mapping_ab", IMGUI_VULKAN)
        self.assertNotIn("DriverId::eMesaTurnip", IMGUI_VULKAN)
        self.assertNotIn("getProperties2<", IMGUI_VULKAN)
        for token in (
            "struct ImGuiUploadPolicy",
            "ResolveImGuiUploadPolicy",
            "ImGuiUploadPolicy imgui_upload",
        ):
            self.assertIn(token, PROFILE)

    def test_descriptor_dedup_ab_is_exact_build_typed_and_default_off(self):
        for token in (
            "struct DescriptorBindingPolicy",
            "ResolveDescriptorBindingPolicy",
            "GpuCompatibilityRoute::Adreno750MesaTurnip",
            "MobileDriverBuild::MesaTurnipA750Cf8e1b81a6",
            "DescriptorBindingPolicy descriptor_binding",
        ):
            self.assertIn(token, PROFILE)
        self.assertIn(
            "descriptor_binding.allow_exact_write_dedup_ab", PIPELINE_COMMON
        )
        self.assertIn(
            "run-enable-exact-vk-descriptor-dedup-a750-turnip",
            DRIVER_OPTIMIZATION,
        )
        self.assertIn(
            "profile_allows_exact_write_dedup_ab && requested",
            DRIVER_OPTIMIZATION,
        )
        self.assertNotIn("DriverId::", PIPELINE_COMMON)

    def test_completion_sidecar_consumer_uses_typed_policy(self):
        for token in (
            "struct CompletionSidecarPolicy",
            "ResolveCompletionSidecarPolicy",
            "CompletionSidecarPolicy completion_sidecar",
        ):
            self.assertIn(token, PROFILE)
        sidecar_start = RASTERIZER.index(
            "bool ExecutorElideTurnipCompletionSidecars"
        )
        sidecar = RASTERIZER[sidecar_start : sidecar_start + 1800]
        self.assertIn("completion_sidecar.diagnostic_allowed", sidecar)
        self.assertIn("completion_sidecar.elide_by_default", sidecar)
        self.assertNotIn("MobileDriverFamily", sidecar)
        self.assertNotIn("exact_mesa_turnip_adreno840", sidecar)

    def test_command_recording_bound_is_exact_typed_and_preserves_other_routes(self):
        for token in (
            "struct CommandSubmissionPolicy",
            "ResolveCommandSubmissionPolicy",
            "CommandSubmissionPolicy command_submission",
            "Adreno750QualcommProprietary",
            "Adreno840QualcommProprietary",
            "Adreno750MesaTurnip",
            "MaliArmProprietary",
        ):
            self.assertIn(token, PROFILE)
        self.assertIn("maximum_deferred_guest_submit_boundaries == 8", PROFILE)
        self.assertIn("maximum_deferred_guest_submit_boundaries == 1", PROFILE)
        self.assertGreaterEqual(
            PROFILE.count("maximum_deferred_guest_submit_boundaries == 64"), 3
        )
        self.assertIn("exact_qcom_a750_single_boundary_requested", PROFILE)
        self.assertIn("ExecutorA750QcomSingleBoundaryRecordingRequested", INSTANCE)
        self.assertIn("EXECUTOR_A750_QCOM_RECORDING_BOUND", INSTANCE)
        self.assertIn(
            "rasterizer->MaximumDeferredGuestSubmitBoundaries()", LIVERPOOL
        )
        self.assertNotIn("constexpr u32 MaxDeferredSubmitBoundaries = 64", LIVERPOOL)

    def test_graphics_signature_reuse_consumes_typed_policy(self):
        for token in (
            "struct GraphicsLookupPolicy",
            "ResolveGraphicsLookupPolicy",
            "GraphicsLookupPolicy graphics_lookup",
            "structural_signature_reuse_default",
            "allow_generation_guarded_reuse_ab",
        ):
            self.assertIn(token, PROFILE)
        start = PIPELINE_CACHE.index(
            "const GraphicsLookupPolicy graphics_lookup_policy"
        )
        window = PIPELINE_CACHE[start : start + 720]
        self.assertIn(
            "instance.GetCapabilityProfile().graphics_lookup", window
        )
        self.assertIn("graphics_lookup_policy.structural_signature_reuse_default", window)
        self.assertIn("graphics_lookup_policy.allow_generation_guarded_reuse_ab", window)
        self.assertNotIn("exact_proprietary_adreno750", window)
        self.assertNotIn("MobileDriverFamily", window)

    def test_vulkan_properties_resolve_the_authoritative_profile_once(self):
        for token in (
            "ResolveMobileGpuModel(properties.vendorID, properties.deviceID)",
            "capability_profile.driver_version = properties.driverVersion",
            "capability_profile.exact_proprietary_adreno840 =",
            "capability_profile.exact_mesa_turnip_adreno750 =",
            "capability_profile.exact_mesa_turnip_adreno840 =",
            "ResolveMobileDriverBuild(",
            "DriverAxis::EffectivePackageSha256()",
            "[EXECUTOR_GPU_COMPATIBILITY_PROFILE]",
        ):
            self.assertIn(token, INSTANCE)

    def test_a840_turnip_consumer_uses_the_profile_not_device_name_parsing(self):
        start = RASTERIZER.index("bool ExecutorElideTurnipCompletionSidecars")
        window = RASTERIZER[start : start + 1800]
        self.assertIn("compatibility.completion_sidecar", window)
        self.assertNotIn("compatibility.mobile_driver_family", window)
        self.assertNotIn("compatibility.exact_mesa_turnip_adreno840", window)
        self.assertNotIn("Adreno 840", window)
        self.assertNotIn("GetDeviceID() ==", window)

    def test_memory_budget_accounting_is_a_typed_exact_route(self):
        for token in (
            "enum class MemoryBudgetAccounting",
            "LegacyRendererShare",
            "ReportedUsageWithInitialBaseline",
            "ReportedBudget70Percent",
            "enum class ResidencyRetirementMode",
            "CoordinatedNonBlocking",
            "CoordinatedSustainedNonBlocking",
            "turnip_a750_budget_baseline_requested",
            "IsExactMesaTurnipAdreno750",
        ):
            self.assertIn(token, PROFILE)
        self.assertIn("run-sm8650-a750-turnip-budget-baseline-ab", INSTANCE)
        self.assertIn("[EXECUTOR_GPU_MEMORY_BUDGET_MODEL]", INSTANCE)
        self.assertIn("GetMemoryPressureUsageLimit()", RASTERIZER)
        self.assertIn("GetMemoryPressureUsageLimit()", TEXTURE_CACHE)
        self.assertIn("GetMemoryPressureUsageLimit()", BUFFER_CACHE)

    def test_bounded_turnip_residency_is_exact_marker_only_and_nonblocking(self):
        for token in (
            "turnip_a750_bounded_residency_requested",
            "MemoryBudgetAccounting::ReportedBudget70Percent",
            "ResidencyRetirementMode::CoordinatedNonBlocking",
            "reported-budget-70-percent",
            "coordinated-nonblocking",
        ):
            self.assertIn(token, PROFILE)
        for token in (
            "run-sm8650-a750-turnip-bounded-residency-ab",
            "[EXECUTOR_A750_TURNIP_BOUNDED_RESIDENCY]",
            "reported_incremental_budget * 70 / 100",
            "residencyRetirement=%s",
        ):
            self.assertIn(token, INSTANCE)
        self.assertIn("coordinated_turnip_a750_pressure", RASTERIZER)
        self.assertIn("proprietary_a750_vma_pressure", RASTERIZER)
        self.assertIn("run_residency_gc || proprietary_a750_vma_pressure", RASTERIZER)
        self.assertIn("aggregate_pressure_policy.cache_pressure_active", TEXTURE_CACHE)
        self.assertIn("aggregate_pressure_policy.cache_pressure_active", BUFFER_CACHE)
        self.assertNotIn("coordinated_nonblocking_retirement", TEXTURE_CACHE)
        self.assertNotIn("coordinated_nonblocking_retirement", BUFFER_CACHE)

    def test_sustained_turnip_retirement_is_bounded_periodic_and_never_waits(self):
        for token in (
            "turnip_a750_sustained_residency_requested",
            "ResidencyRetirementMode::CoordinatedSustainedNonBlocking",
            "coordinated-sustained-nonblocking",
        ):
            self.assertIn(token, PROFILE)
        for token in (
            "run-sm8650-a750-turnip-sustained-residency-drain-ab",
            "[EXECUTOR_A750_TURNIP_SUSTAINED_RESIDENCY]",
            "cadenceMs=1000 waited=0",
        ):
            self.assertIn(token, INSTANCE)
        for token in (
            "SustainedRetirementIntervalNs = 1'000'000'000",
            "SustainedDrainIntervalNs = 1'000'000'000",
            "sustained_turnip_a750_retirement",
            "scheduler.Flush()",
        ):
            self.assertIn(token, RASTERIZER)
        self.assertNotIn("scheduler.Finish()", RASTERIZER[RASTERIZER.index(
            "SustainedDrainIntervalNs") : RASTERIZER.index(
            "SustainedDrainIntervalNs") + 2500])

    def test_bounded_residency_cannot_leak_to_a840_unknown_turnip_or_mali(self):
        for neighbor in (
            "Adreno840DeviceId, MobileDriverBuild::MesaTurnipA750Cf8e1b81a6,\n"
            "                  false, false, false, false, true)\n"
            "                  .residency_retirement == ResidencyRetirementMode::LegacyCacheLocal",
            "MobileDriverFamily::ArmProprietary, ArmVendorId, 0,\n"
            "                  MobileDriverBuild::MesaTurnipA750Cf8e1b81a6,\n"
            "                  false, false, false, false, true)\n"
            "                  .budget_accounting == MemoryBudgetAccounting::LegacyRendererShare",
            "Adreno750DeviceId, MobileDriverBuild::Unknown,\n"
            "                  false, false, false, false, true)\n"
            "                  .residency_retirement == ResidencyRetirementMode::LegacyCacheLocal",
        ):
            self.assertIn(neighbor, PROFILE)

    def test_bda_topology_is_typed_exact_and_neighbor_safe(self):
        for token in (
            "enum class BdaPageTableMode",
            "FlatResident",
            "FlatSparse",
            "SegmentedSparse",
            "struct BdaPageTablePolicy",
            "ResolveBdaPageTablePolicy",
            "MaximumBdaDescriptorSegments = 4",
            "max_storage_buffer_range != MinimumBdaDescriptorSegmentSize",
            "MobileDriverBuild::MesaTurnipA750Da14d65e44Ib32RolloverBoStats",
        ):
            self.assertIn(token, PROFILE)
        self.assertIn(
            "run-sm8650-a750-turnip-bda-segmented-sparse-ab", INSTANCE
        )
        self.assertIn(
            "run-sm8650-a750-turnip-bda-flat-sparse-ab", INSTANCE
        )
        self.assertIn("flatSparseRequested=%u", INSTANCE)
        self.assertIn(".mode == BdaPageTableMode::FlatSparse", PROFILE)
        self.assertIn(
            "run-sm8650-a750-qcom-bda-segmented-sparse-ab", INSTANCE
        )
        self.assertIn("descriptorSegments=%u", INSTANCE)
        for exact_neighbor in (
            "MobileDriverFamily::MesaTurnip, QualcommVendorId,\n                  Adreno840DeviceId,\n                  MobileDriverBuild::MesaTurnipA750Da14d65e44Ib32RolloverBoStats",
            "MobileDriverFamily::ArmProprietary, ArmVendorId, 0,\n                  MobileDriverBuild::Unknown",
        ):
            start = PROFILE.index(exact_neighbor)
            window = PROFILE[start : start + 360]
            self.assertIn("BdaPageTableMode::FlatResident", window)
        self.assertIn(
            "MobileDriverFamily::QualcommProprietary, QualcommVendorId,\n"
            "                  Adreno750DeviceId, MobileDriverBuild::Unknown, false, true, false",
            PROFILE,
        )
        for unsupported_range in (192, 256, 512):
            exact_case = (
                "MobileDriverBuild::MesaTurnipA750Da14d65e44Ib32RolloverBoStats,\n"
                f"                  true, false, false, true, {unsupported_range}ULL * 1024ULL * 1024ULL"
            )
            start = PROFILE.index(exact_case)
            self.assertIn(
                "BdaPageTableMode::FlatResident", PROFILE[start : start + 300]
            )
        unknown_turnip = (
            "Adreno750DeviceId, MobileDriverBuild::Unknown, true, false, false,\n"
            "                  true, 128ULL * 1024ULL * 1024ULL"
        )
        start = PROFILE.index(unknown_turnip)
        self.assertIn("BdaPageTableMode::FlatResident", PROFILE[start : start + 300])

    def test_qti_a750_legacy_descriptor_batch_is_bounded_and_neighbor_safe(self):
        self.assertIn(
            "run-sm8650-a750-qcom-bounded-descriptor-batch-ab", INSTANCE
        )
        self.assertIn("u8 legacy_allocation_batch{32}", PROFILE)
        self.assertIn("bounded_qti_a750_batch_requested", PROFILE)
        self.assertIn(".legacy_allocation_batch == 4", PROFILE)
        self.assertIn(".legacy_allocation_batch == 32", PROFILE)

    def test_budget_candidate_cannot_leak_to_a840_qti_or_mali(self):
        for exact_neighbor in (
            "MobileDriverFamily::MesaTurnip,\n                                        QualcommVendorId, Adreno840DeviceId,\n                                        MobileDriverBuild::Unknown, true",
            "MobileDriverFamily::QualcommProprietary,\n                                        QualcommVendorId, Adreno750DeviceId,\n                                        MobileDriverBuild::Unknown, true",
            "MobileDriverFamily::ArmProprietary,\n                                        ArmVendorId, 0, MobileDriverBuild::Unknown,\n                                        true",
        ):
            start = PROFILE.index(exact_neighbor)
            window = PROFILE[start : start + 360]
            self.assertIn("MemoryBudgetAccounting::LegacyRendererShare", window)

    def test_turnip_build_candidate_is_exact_but_product_default_is_quarantined(self):
        for token in (
            "enum class MobileDriverBuild",
            "MesaTurnipA750Cf8e1b81a6",
            "MesaTurnipA750Cf8e1b81a6DriverVersion",
            "MesaTurnipA750Cf8e1b81a6Sha256",
            "MesaTurnipA750Da14d65e44",
            "MesaTurnipA750Da14d65e44DriverVersion",
            "MesaTurnipA750Da14d65e44Sha256",
            "MesaTurnipA750Da14d65e44Ib32Rollover",
            "MesaTurnipA750Da14d65e44Ib32RolloverDriverVersion",
            "MesaTurnipA750Da14d65e44Ib32RolloverSha256",
            "HasCandidateA750TurnipComposedMemoryPolicy",
            "HasProductDefaultA750TurnipComposedMemoryPolicy",
            "MobileDriverBuildPolicyState",
            "ResolveMobileDriverBuildPolicyState",
        ):
            self.assertIn(token, PROFILE)
        self.assertIn(
            "run-disable-sm8650-a750-turnip-composed-memory", INSTANCE
        )
        self.assertIn(
            "candidateBuild=%u productDefault=%u disabled=%u driverBuild=%s policyState=%s",
            INSTANCE,
        )
        allocator_start = INSTANCE.index("void Instance::CreateAllocator()")
        allocator = INSTANCE[allocator_start : allocator_start + 3400]
        self.assertIn(
            "exact_turnip_a750 &&\n          (routed_block_size != 0 || "
            "a750_turnip_block64_requested)",
            allocator,
        )

    def test_upload_reservation_policy_is_typed_exact_and_marker_gated(self):
        for token in (
            "enum class UploadReservationMode",
            "FullSpan",
            "ExactDirtyRanges",
            "UploadReservationMode upload_reservation",
            "enum class UploadReservationScope",
            "AllCpuDirtyUploads",
            "UploadReservationScope upload_reservation_scope",
            "turnip_a750_sparse_upload_requested",
            "HasCandidateA750TurnipComposedMemoryPolicy(driver_build)",
        ):
            self.assertIn(token, PROFILE)
        self.assertIn(
            "run-sm8650-a750-turnip-sparse-upload-reservation-ab", INSTANCE
        )
        self.assertIn("[EXECUTOR_A750_TURNIP_UPLOAD_RESERVATION]", INSTANCE)
        self.assertIn("uploadReservation=%s", INSTANCE)
        self.assertIn("uploadReservationScope=%s", INSTANCE)
        self.assertIn(
            "upload_policy.upload_reservation ==",
            BUFFER_CACHE,
        )
        self.assertIn("Vulkan::UploadReservationMode::ExactDirtyRanges", BUFFER_CACHE)
        self.assertIn("Vulkan::UploadReservationScope::AllCpuDirtyUploads", BUFFER_CACHE)
        self.assertIn("exact_dirty_upload_reservation", BUFFER_CACHE)
        self.assertNotIn(
            "instance.GetCapabilityProfile().exact_mesa_turnip_adreno750 &&\n"
            "        (fex_buffer_capture || fex_snapshot_boundary)",
            BUFFER_CACHE,
        )

    def test_sparse_upload_candidate_cannot_leak_to_a840_unknown_turnip_or_mali(self):
        for exact_neighbor in (
            "MobileDriverFamily::MesaTurnip,\n                                        QualcommVendorId, Adreno840DeviceId,\n                                        MobileDriverBuild::MesaTurnipA750Cf8e1b81a6,\n                                        false, false, true",
            "MobileDriverFamily::ArmProprietary,\n                                        ArmVendorId, 0,\n                                        MobileDriverBuild::MesaTurnipA750Cf8e1b81a6,\n                                        false, false, true",
            "MobileDriverFamily::MesaTurnip,\n                                        QualcommVendorId, Adreno750DeviceId,\n                                        MobileDriverBuild::Unknown, false, false, true",
        ):
            start = PROFILE.index(exact_neighbor)
            window = PROFILE[start : start + 390]
            self.assertIn("UploadReservationMode::FullSpan", window)

    def test_all_upload_scope_is_exact_route_and_marker_gated(self):
        self.assertIn(
            "UploadReservationScope::AllCpuDirtyUploads", PROFILE
        )
        qti = PROFILE.index(
            "MobileDriverFamily::QualcommProprietary,\n"
            "                                        QualcommVendorId, Adreno750DeviceId,\n"
            "                                        MobileDriverBuild::Unknown)\n"
            "                  .upload_reservation_scope"
        )
        self.assertIn(
            "UploadReservationScope::CaptureBoundaryOnly", PROFILE[qti : qti + 300]
        )
        self.assertIn(
            "run-sm8650-a750-qcom-all-upload-reservation-ab", INSTANCE
        )
        self.assertIn("[EXECUTOR_A750_QCOM_UPLOAD_RESERVATION]", INSTANCE)
        self.assertIn("qcom_a750_all_upload_reservation_requested", PROFILE)
        qti_candidate = PROFILE.index(
            "Adreno750DeviceId, MobileDriverBuild::Unknown, false, false,\n"
            "                  false, false, false, true)\n"
            "                  .upload_reservation_scope"
        )
        self.assertIn(
            "UploadReservationScope::AllCpuDirtyUploads",
            PROFILE[qti_candidate : qti_candidate + 300],
        )
        qti_a840 = PROFILE.index(
            "Adreno840DeviceId, MobileDriverBuild::Unknown, false, false,\n"
            "                  false, false, false, true)\n"
            "                  .upload_reservation_scope"
        )
        self.assertIn(
            "UploadReservationScope::CaptureBoundaryOnly",
            PROFILE[qti_a840 : qti_a840 + 300],
        )
        exact = PROFILE.index(
            "MobileDriverBuild::MesaTurnipA750Cf8e1b81a6,\n"
            "                                        false, false, true)\n"
            "                  .upload_reservation_scope"
        )
        self.assertIn(
            "UploadReservationScope::AllCpuDirtyUploads", PROFILE[exact : exact + 300]
        )
        exact_bundled = PROFILE.index(
            "MobileDriverBuild::MesaTurnipA750Da14d65e44,\n"
            "                                        false, false, true)\n"
            "                  .upload_reservation_scope"
        )
        self.assertIn(
            "UploadReservationScope::AllCpuDirtyUploads",
            PROFILE[exact_bundled : exact_bundled + 300],
        )

    def test_qcom_structural_reuse_opt_out_is_typed_and_exact(self):
        self.assertIn(
            "run-sm8650-a750-qcom-disable-structural-reuse-ab", INSTANCE
        )
        self.assertIn(
            "qcom_a750_structural_reuse_disabled", PROFILE
        )
        self.assertIn("[EXECUTOR_A750_QCOM_STRUCTURAL_REUSE]", INSTANCE)
        exact = PROFILE.index(
            "GpuCompatibilityRoute::Adreno750QualcommProprietary, true)\n"
            "                   .structural_signature_reuse_default"
        )
        self.assertIn("static_assert(!ResolveGraphicsLookupPolicy", PROFILE[exact - 90 : exact])

    def test_qcom_pipeline_quirk_opt_outs_are_independent_and_exact(self):
        for marker in (
            "run-sm8650-a750-qcom-disable-formatted-buffer-defer-ab",
            "run-sm8650-a750-qcom-disable-aliased-shared-u64-lowering-ab",
        ):
            self.assertIn(marker, INSTANCE)
        self.assertIn("[EXECUTOR_A750_QCOM_PIPELINE_QUIRKS]", INSTANCE)
        self.assertIn("const bool defer_formatted_buffer_types = true", PROFILE)
        self.assertIn("const bool lower_aliased_shared_u64 = true", PROFILE)
        self.assertIn(
            "Adreno750DeviceId, false, false) == 0", PROFILE
        )
        for neighbor in (
            "MobileDriverFamily::MesaTurnip, QualcommVendorId,\n"
            "                  Adreno750DeviceId) == 0",
            "MobileDriverFamily::ArmProprietary, 0x13b5,\n"
            "                   Adreno750DeviceId) == 0",
        ):
            self.assertIn(neighbor, PROFILE)


if __name__ == "__main__":
    unittest.main()
