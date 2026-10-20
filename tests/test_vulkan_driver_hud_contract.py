import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
ACTIVITY = (
    ROOT
    / "android-app/app/src/main/java/app/lsx4/android/MainActivity.java"
).read_text(encoding="utf-8")
BUILD_GRADLE = (ROOT / "android-app/app/build.gradle").read_text(encoding="utf-8")


class VulkanDriverHudContractTests(unittest.TestCase):
    def test_hud_places_effective_driver_before_brand(self):
        driver = ACTIVITY.index("debugHudPanel.addView(debugHudDriver")
        brand = ACTIVITY.index("debugHudPanel.addView(debugHudBrand")
        self.assertLess(driver, brand)

    def test_hud_reports_actual_driver_and_visible_fallback(self):
        self.assertIn('json.optString("effective", "")', ACTIVITY)
        self.assertIn('json.optString("deviceName", "")', ACTIVITY)
        self.assertIn('label.append("Turnip")', ACTIVITY)
        self.assertIn('label.append("Qualcomm")', ACTIVITY)
        self.assertIn('label.append("  VK ").append(apiVersion)', ACTIVITY)
        self.assertIn('label.append("  TURNIP FALLBACK")', ACTIVITY)

    def test_system_driver_is_not_unconditionally_labeled_qualcomm(self):
        self.assertNotIn(
            '} else if ("system".equalsIgnoreCase(effective)', ACTIVITY
        )
        self.assertIn('contains("adreno")', ACTIVITY)
        self.assertIn('String systemLabel = !driverName.isEmpty()', ACTIVITY)

    def test_locked_actual_driver_properties_override_effective_route(self):
        classifier = ACTIVITY.index("boolean hasActualDriverProperties")
        body = ACTIVITY[classifier : classifier + 2200]
        self.assertIn(
            "boolean actualPropertiesAuthoritative = selectionLocked", body
        )
        self.assertIn("&& hasActualDriverProperties", body)
        self.assertIn("boolean actualTurnip", body)
        self.assertIn('driverNameLower.contains("mesa")', body)
        self.assertIn('driverInfoLower.contains("turnip")', body)
        self.assertIn("boolean actualQualcomm = !actualTurnip", body)
        self.assertIn('driverNameLower.contains("qualcomm")', body)
        self.assertIn('driverInfoLower.contains("adreno")', body)
        self.assertIn(
            "boolean turnip = actualPropertiesAuthoritative\n"
            "                    ? actualTurnip\n"
            "                    : effectiveTurnip",
            body,
        )
        self.assertIn(
            "boolean qualcomm = actualPropertiesAuthoritative\n"
            "                    ? actualQualcomm",
            body,
        )

    def test_locked_route_driver_disagreement_is_visible(self):
        mismatch = ACTIVITY.index("boolean actualDriverRouteMismatch")
        body = ACTIVITY[mismatch : mismatch + 500]
        self.assertIn("actualPropertiesAuthoritative", body)
        self.assertIn("actualQualcomm && effectiveTurnip", body)
        self.assertIn('label.append("  DRIVER MISMATCH")', body)

    def test_hud_refreshes_status_after_vulkan_device_creation(self):
        self.assertIn("RuntimeBridge.vulkanDriverStatus()", ACTIVITY)
        self.assertIn(
            "describeVulkanDriverForHud(runtimeHudVulkanStatus)", ACTIVITY
        )

    def test_unlocked_turnip_preflight_is_not_reported_as_fallback(self):
        self.assertIn('json.optBoolean("selectionLocked", false)', ACTIVITY)
        self.assertIn('return "GPU Turnip pending..."', ACTIVITY)
        fallback = ACTIVITY.index('label.append("  TURNIP FALLBACK")')
        self.assertIn("selectionLocked", ACTIVITY[fallback - 180 : fallback])

    def test_diagnostic_turnip_can_be_packaged_without_replacing_pinned_artifact(self):
        self.assertIn('gradleProperty("lsx4TurnipDriver")', BUILD_GRADLE)
        self.assertIn('gradleProperty("lsx4TurnipDriverSha256")', BUILD_GRADLE)
        self.assertIn('turnipPackagedName = "libvulkan_lsx4_turnip.so"', BUILD_GRADLE)
        self.assertIn("rename { turnipPackagedName }", BUILD_GRADLE)
        self.assertIn("verifyTurnipDriverInApks", BUILD_GRADLE)
        self.assertIn(
            'String expectedEntry = "lib/arm64-v8a/${turnipPackagedName}"',
            BUILD_GRADLE,
        )
        self.assertIn(
            'orElse("b56a4cec9af33af61ec068efb1d8673b54bb325635ca7a2300db77df82d752cd")',
            BUILD_GRADLE,
        )


if __name__ == "__main__":
    unittest.main()
