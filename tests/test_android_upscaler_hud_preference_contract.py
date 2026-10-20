import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
ACTIVITY = (
    ROOT / "android-app/app/src/main/java/app/lsx4/android/MainActivity.java"
).read_text(encoding="utf-8")
SETTINGS = (
    ROOT / "android-app/app/src/main/java/app/lsx4/android/SettingsActivity.java"
).read_text(encoding="utf-8")
STRINGS = (ROOT / "android-app/app/src/main/res/values/strings.xml").read_text(
    encoding="utf-8"
)
STRINGS_RU = (
    ROOT / "android-app/app/src/main/res/values-ru/strings.xml"
).read_text(encoding="utf-8")


class AndroidUpscalerHudPreferenceContractTests(unittest.TestCase):
    def test_other_page_exposes_default_off_upscaler_hud_checkbox(self):
        self.assertIn(
            'K_PERF_HUD_UPSCALER_INFO = "perf_hud_upscaler_info"', SETTINGS
        )
        other = SETTINGS.index("private void buildOther")
        other_body = SETTINGS[other : other + 700]
        self.assertIn("R.string.debug_hud_upscaler_info", other_body)
        self.assertIn("K_PERF_HUD_UPSCALER_INFO, false", other_body)
        self.assertIn('name="debug_hud_upscaler_info"', STRINGS)
        self.assertIn('name="debug_hud_upscaler_info"', STRINGS_RU)

    def test_upscaler_hud_setting_crosses_into_game_process(self):
        snapshot = ACTIVITY.index("private static final String[] CROSS_PROCESS_SETTING_KEYS")
        snapshot_body = ACTIVITY[snapshot : snapshot + 2500]
        self.assertIn("SettingsActivity.K_PERF_HUD_UPSCALER_INFO", snapshot_body)
        self.assertIn(
            "SettingsActivity.K_PERF_HUD_UPSCALER_INFO, false", ACTIVITY
        )
        self.assertIn(
            "SettingsActivity.K_PERF_HUD_UPSCALER_INFO.equals(key)", ACTIVITY
        )

    def test_upscaler_line_is_hidden_unless_checkbox_is_enabled(self):
        install = ACTIVITY.index('debugHudFsr.setText("UPSCALER --")')
        install_body = ACTIVITY[install : install + 800]
        self.assertIn("!ps5Runtime && showDebugHudUpscalerInfo", install_body)

        update = ACTIVITY.index("private void updateFsrProofHud")
        update_body = ACTIVITY[update : update + 700]
        self.assertIn(
            "final boolean visible = !ps5Runtime && showDebugHudUpscalerInfo",
            update_body,
        )
        self.assertIn(
            "debugHudFsr.setVisibility(visible ? View.VISIBLE : View.GONE)",
            update_body,
        )
        self.assertLess(update_body.index("if (!visible)"), update_body.index("current == null"))


if __name__ == "__main__":
    unittest.main()
