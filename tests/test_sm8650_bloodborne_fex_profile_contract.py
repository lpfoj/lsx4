from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


def read(relative: str) -> str:
    return (ROOT / relative).read_text(encoding="utf-8")


class RuntimeOptimizationProfileAxisContractTest(unittest.TestCase):
    def test_launcher_passes_title_hardware_and_driver_to_optional_v3_abi(self) -> None:
        main = read("android-app/app/src/main/java/app/lsx4/android/MainActivity.java")
        manager = read(
            "android-app/app/src/main/java/app/lsx4/android/VulkanDriverManager.java"
        )
        java_bridge = read("android-app/app/src/main/java/app/lsx4/android/RuntimeBridge.java")
        native_bridge = read("android-app/app/src/main/cpp/runtime_bridge.cpp")

        self.assertIn('Pattern.compile("(?i)(CUSA|PPSA)[0-9]{5}")', main)
        self.assertIn("runtimeHardwareProfile, runtimeDriverFamily", main)
        self.assertIn("runtimeHardwareProfile(Context context, String driverStatus)", manager)
        self.assertIn("runtimeDriverFamily(String driverStatus, String hardwareProfile)", manager)
        self.assertIn("runtimeHardwareProfile.equals(previousHardware)", main)
        self.assertIn(
            'if ("turnip".equals(requested) && !selectionLocked)', manager
        )
        self.assertIn('return "system-unknown";', manager)
        self.assertIn(
            'refreshGuestCpuDriverAxisAfterVulkanLock(\n'
            '                        "turnip-health-confirm")',
            main,
        )
        self.assertIn(
            "SetGuestCpuBackend updates the immutable profile axes", main
        )
        self.assertIn("String driverVersion", java_bridge)
        self.assertIn("executor_lsx4_runtime_set_guest_cpu_backend_v3", native_bridge)
        self.assertIn("setter_v3 || setter_v2 || setter", native_bridge)

    def test_profile_is_composed_from_independent_fail_closed_axes(self) -> None:
        runtime = read("src/executor/native_runtime_api.cpp")

        for value in (
            "adreno-750",
            "adreno-840",
            "mali",
            "qualcomm-proprietary",
            "mesa-turnip",
            "arm-proprietary",
            "bloodborne-heavy-render-v1",
            "shared-semantics-v1",
        ):
            self.assertIn(value, runtime)
        self.assertIn("Common::IsSnapdragon8Gen3Host()", runtime)
        self.assertIn("a840-sve128,a840-turnip-completion", runtime)
        self.assertNotIn("d31-multiblock-runtime-create", runtime)
        self.assertIn('"a840-sve128,a840-turnip-completion"', runtime)

    def test_rejected_a750_d31_is_not_automatically_enabled(self) -> None:
        runtime = read("src/executor/native_runtime_api.cpp")

        self.assertNotIn("run-enable-sm8650-bloodborne-d31-profile-ab", runtime)
        self.assertNotIn("sm8650_bloodborne_d31_profile", runtime)
        self.assertIn(
            'root / "run-enable-fex-multiblock"', runtime
        )

    def test_vulkan_title_routing_remains_in_capability_profile(self) -> None:
        runtime = read("src/executor/native_runtime_api.cpp")
        capability = read(
            "funnel-arm/src/video_core/renderer_vulkan/vk_capability_profile.h"
        )

        self.assertIn("Adreno840DeviceId", capability)
        self.assertIn("IsExactProprietaryAdreno750", capability)
        self.assertNotIn("CUSA03173", capability)
        self.assertNotIn("bloodborne", capability.lower())
        self.assertIn("[EXECUTOR_OPTIMIZATION_PROFILE_AXIS]", runtime)


if __name__ == "__main__":
    unittest.main()
