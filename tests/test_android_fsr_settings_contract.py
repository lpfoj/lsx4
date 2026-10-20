import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def source(relative_path: str) -> str:
    return (ROOT / relative_path).read_text(encoding="utf-8")


class AndroidFsrSettingsContractTest(unittest.TestCase):
    def test_android_preferences_reach_the_ps4_runtime(self):
        settings = source(
            "android-app/app/src/main/java/app/lsx4/android/SettingsActivity.java"
        )
        activity = source(
            "android-app/app/src/main/java/app/lsx4/android/MainActivity.java"
        )
        bridge = source(
            "android-app/app/src/main/java/app/lsx4/android/RuntimeBridge.java"
        )

        for key in ("K_FSR_ENABLED", "K_RCAS_ENABLED",
                    "K_UPSCALED_OUTPUT_RES_MODE", "K_UPSCALER_MODE"):
            self.assertIn(key, settings)
            self.assertIn(f"SettingsActivity.{key}", activity)
        self.assertIn("MANAGED_OPTIMIZATION_FSR = 20", bridge)
        self.assertIn("MANAGED_OPTIMIZATION_RCAS = 21", bridge)
        self.assertIn("new boolean[22]", activity)
        self.assertIn("MANAGED_OPTIMIZATION_FSR] = fsrEnabled", activity)
        self.assertIn("MANAGED_OPTIMIZATION_RCAS] = rcasEnabled", activity)

    def test_presenter_honours_fsr_and_rcas_preferences(self):
        native = source("src/executor/native_runtime_api.cpp")
        policy = source("funnel-arm/src/video_core/mobile_gpu_optimization.cpp")
        presenter = source(
            "funnel-arm/src/video_core/renderer_vulkan/vk_presenter.cpp"
        )

        self.assertIn("SetManagedMobileFsrEnabled(active)", native)
        self.assertIn("SetManagedMobileRcasEnabled(active)", native)
        self.assertIn("std::atomic_bool fsr_enabled{true}", policy)
        self.assertIn("std::atomic_bool rcas_enabled{true}", policy)
        self.assertIn("IsManagedMobileFsrEnabled() &&", presenter)
        self.assertIn("IsManagedMobileRcasEnabled()", presenter)

    def test_hud_proves_actual_easu_and_rcas_dispatches(self):
        policy = source("funnel-arm/src/video_core/mobile_gpu_optimization.cpp")
        fsr_pass = source(
            "funnel-arm/src/video_core/renderer_vulkan/host_passes/fsr_pass.cpp"
        )
        runtime = source("src/executor/native_runtime_api.cpp")
        activity = source(
            "android-app/app/src/main/java/app/lsx4/android/MainActivity.java"
        )
        proof = source(
            "android-app/app/src/main/java/app/lsx4/android/ProofMetricsReceiver.java"
        )

        wants_fsr = policy.split("bool MobileGpuWantsFsr", 1)[1].split(
            "void RecordMobileGpuFsrDecision", 1
        )[0]
        self.assertNotIn("fsr_frames.fetch_add", wants_fsr)
        self.assertIn("RecordMobileGpuFsrDecision(true, settings.use_rcas", fsr_pass)
        self.assertIn("state.fsr_frames.fetch_add(1", policy)
        self.assertIn("state.rcas_frames.fetch_add(1", policy)
        self.assertIn("kValueCount = 166", runtime)
        self.assertIn('"%s ACTIVE · %s · %,d/s%s%s%s"', activity)
        self.assertIn('hudCounters.put("fsrEasuDispatchFrames"', proof)
        self.assertIn('hudCounters.put("fsrRcasDispatchFrames"', proof)
        self.assertIn('hudCounters.put("upscalerEffectiveMode"', proof)
        self.assertIn('hudCounters.put("upscalerDispatchFrames"', proof)

    def test_upscalers_use_an_explicit_lower_source_and_higher_output_pair(self):
        settings = source(
            "android-app/app/src/main/java/app/lsx4/android/SettingsActivity.java"
        )
        native = source("src/executor/native_runtime_api.cpp")
        activity = source(
            "android-app/app/src/main/java/app/lsx4/android/MainActivity.java"
        )
        rasterizer = source(
            "funnel-arm/src/video_core/renderer_vulkan/vk_rasterizer.cpp"
        )
        self.assertIn("higherUpscaledOutputModes(int sourceMode)", settings)
        self.assertIn("RES_180P, RES_270P, RES_360P, RES_544P", settings)
        self.assertIn("renderUpscaledOutputSelector", settings)
        self.assertIn("sceneRenderScalePercentFor(int mode)", activity)
        self.assertIn("return Math.max(20, Math.min(100, percent))", activity)
        self.assertIn('"version=4\\nfsr_enabled="', activity)
        self.assertIn('"\\nupscaler_mode="', activity)
        self.assertIn('"\\nsource_width="', activity)
        self.assertIn('"\\noutput_width="', activity)
        self.assertIn("AndroidInternalResolutionForUpscaler", native)
        self.assertIn("LoadAndroidRenderResolutionSelection", native)
        self.assertIn('"android_upscaler_explicit_pair"', native)
        self.assertIn("physical_width, std::min(static_cast<u32>(state.width), 960u)", rasterizer)
        self.assertIn("physical_height, std::min(static_cast<u32>(state.height), 544u)", rasterizer)

    def test_every_backend_has_a_distinct_implementation_route(self):
        settings = source(
            "android-app/app/src/main/java/app/lsx4/android/SettingsActivity.java"
        )
        spatial = source("funnel-arm/src/video_core/host_shaders/spatial_upscale.comp")
        presenter = source("funnel-arm/src/video_core/renderer_vulkan/vk_presenter.cpp")
        arm_asr = source(
            "funnel-arm/src/video_core/renderer_vulkan/host_passes/arm_asr_pass.cpp"
        )
        for mode in ("UPSCALER_BILINEAR", "UPSCALER_SHARP_BILINEAR",
                     "UPSCALER_BICUBIC", "UPSCALER_FSR1", "UPSCALER_SGSR1",
                     "UPSCALER_NIS", "UPSCALER_SGSR2", "UPSCALER_ARM_ASR"):
            self.assertIn(mode, settings)
        self.assertIn("SampleSharpBilinear", spatial)
        self.assertIn("SampleCatmullRom", spatial)
        self.assertIn("SampleSgsr1", spatial)
        self.assertIn("Sgsr2Pass::TemporalInputs", presenter)
        self.assertIn("ffxmFsr2ContextDispatch", arm_asr)

    def test_each_backend_receives_only_its_own_tuning_contract(self):
        settings = source(
            "android-app/app/src/main/java/app/lsx4/android/SettingsActivity.java"
        )
        activity = source(
            "android-app/app/src/main/java/app/lsx4/android/MainActivity.java"
        )
        native = source("src/executor/native_runtime_api.cpp")
        for key in (
            "K_SHARP_BILINEAR_STRENGTH",
            "K_BICUBIC_KERNEL",
            "K_FSR1_RCAS_SHARPNESS",
            "K_SGSR1_EDGE_SHARPNESS",
            "K_NIS_SHARPNESS",
            "K_SGSR2_TEMPORAL_STABILITY",
            "K_ARM_ASR_SHARPENING",
            "K_ARM_ASR_SHARPNESS",
        ):
            self.assertIn(key, settings)
            self.assertIn(f"SettingsActivity.{key}", activity)
        self.assertIn("renderUpscalerParameterControls", settings)
        self.assertIn("case UPSCALER_FSR1", settings)
        self.assertIn("case UPSCALER_SGSR1", settings)
        self.assertIn("case UPSCALER_SGSR2", settings)
        self.assertIn("case UPSCALER_ARM_ASR", settings)
        self.assertIn('"version=4\\nfsr_enabled="', activity)
        for field in (
            "sharp_bilinear_strength_milli",
            "bicubic_kernel",
            "fsr1_rcas_sharpness_milli",
            "sgsr1_edge_sharpness_milli",
            "nis_sharpness_milli",
            "sgsr2_temporal_stability_milli",
            "arm_asr_sharpening",
            "arm_asr_sharpness_milli",
        ):
            self.assertIn(field, activity)
            self.assertIn(field, native)

    def test_android_upscaler_output_stays_locked_to_the_surface_extent(self):
        presenter = source(
            "funnel-arm/src/video_core/renderer_vulkan/vk_presenter.cpp"
        )
        constructor = presenter.split("Presenter::Presenter", 1)[1].split(
            "Presenter::~Presenter", 1
        )[0]
        sizing = presenter.split("void Presenter::SetExpectedGameSize", 1)[1]
        self.assertIn("initial_extent = swapchain.GetExtent()", constructor)
        self.assertIn("expected_frame_width = initial_extent.width", constructor)
        self.assertIn("surface_extent = swapchain.GetExtent()", sizing)
        self.assertIn("expected_frame_width = surface_extent.width", sizing)
        self.assertIn("expected_frame_height = surface_extent.height", sizing)

    def test_vivo_install_has_package_installer_attribution(self):
        installer = source("scripts/install-vivo-apk.ps1")
        self.assertIn("$installerPackage = 'com.android.packageinstaller'", installer)
        self.assertIn("'pm', 'set-installer'", installer)
        self.assertIn("installerPackageName=", installer)
        self.assertNotIn("'install', '-r', '-i'", installer)

    def test_bloodborne_measurement_normalizes_windows_line_endings(self):
        measurement = source("scripts/measure-bloodborne-gameplay-vivo.ps1")
        self.assertIn('.Replace("`r`n", "`n").Replace("`r", "")', measurement)


if __name__ == "__main__":
    unittest.main()
