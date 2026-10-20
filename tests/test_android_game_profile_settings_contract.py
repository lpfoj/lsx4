import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
JAVA = ROOT / "android-app/app/src/main/java/app/lsx4/android"
PROFILE = (JAVA / "GameProfileSettings.java").read_text(encoding="utf-8")
LIBRARY = (JAVA / "LauncherUi.java").read_text(encoding="utf-8")
SETTINGS = (JAVA / "SettingsActivity.java").read_text(encoding="utf-8")
MAIN = (JAVA / "MainActivity.java").read_text(encoding="utf-8")
EDITOR = (JAVA / "OverlayEditorActivity.java").read_text(encoding="utf-8")
STRINGS = (
    ROOT / "android-app/app/src/main/res/values/strings.xml"
).read_text(encoding="utf-8")
STRINGS_RU = (
    ROOT / "android-app/app/src/main/res/values-ru/strings.xml"
).read_text(encoding="utf-8")


class AndroidGameProfileSettingsContractTests(unittest.TestCase):
    def test_every_library_item_has_a_trailing_individual_settings_button(self):
        row = LIBRARY.split("private View gameRow", 1)[1].split(
            "private void openGameSettings", 1
        )[0]
        self.assertIn('label("\\u2630"', row)
        self.assertIn("gameSettings.setOnClickListener", row)
        self.assertLess(row.index("row.addView(endHost"), row.index("row.addView(gameSettings"))
        self.assertIn("GameProfileSettings.EXTRA_GAME_TITLE_ID", LIBRARY)
        self.assertIn("GameProfileSettings.EXTRA_GAME_TITLE", LIBRARY)

    def test_sparse_profile_inherits_global_values_and_scopes_only_game_options(self):
        self.assertIn('PROFILE_PREFIX = "game.profile."', PROFILE)
        self.assertIn("base.contains(scoped)", PROFILE)
        self.assertIn("return isIndividualKey(key) ? prefix + key : key", PROFILE)
        self.assertIn("SettingsActivity.K_RES_MODE", PROFILE)
        self.assertIn("SettingsActivity.K_VULKAN_DRIVER_SELECTION", PROFILE)
        self.assertIn("SettingsActivity.K_AUDIO", PROFILE)
        self.assertIn("OverlayLayoutStore.PREFIX", PROFILE)
        self.assertIn("HardwareTriggerMapper.PREFIX", PROFILE)
        self.assertNotIn("SettingsActivity.K_TEST_MODE", PROFILE)
        self.assertNotIn("SettingsActivity.K_REMOTE_DIAGNOSTICS", PROFILE)

    def test_game_launch_consumes_effective_profile_for_every_runtime_axis(self):
        self.assertIn("SettingsActivity.prefsForGame(\n                this, launchTitleId())", MAIN)
        self.assertIn("GraphicsBackend.selected(this, appSettings)", MAIN)
        self.assertIn("applyManagedOptimizations()", MAIN)
        managed = MAIN.split("private void applyManagedOptimizations", 1)[1].split(
            "private void initializeRuntime", 1
        )[0]
        self.assertIn("SharedPreferences preferences = gamePreferences()", managed)
        self.assertIn("GameCacheManager.applyStoredSetting(\n                this, gamePreferences())", MAIN)
        self.assertIn("requestedFsrEnabled(gamePreferences())", MAIN)

    def test_game_settings_screen_covers_meaningful_categories_and_has_scope_note(self):
        game_root = SETTINGS.split("private void buildGameRoot", 1)[1].split(
            "private void buildGraphics", 1
        )[0]
        for screen in (
            '"graphics"',
            '"managed_optimizations"',
            '"cpu_tuning"',
            '"audio"',
            '"controls"',
            '"cache"',
            '"other"',
        ):
            self.assertIn(screen, game_root)
        self.assertIn("R.string.game_profile_scope_note", game_root)
        self.assertIn("GameProfileSettings.clearOverrides", game_root)
        self.assertIn("CpuTuningSettings.clearGameProfile", game_root)

    def test_game_cpu_settings_do_not_render_a_redundant_enable_control(self):
        cpu_controls = SETTINGS.split(
            "private void renderCpuTuningControls", 1
        )[1].split("private CpuTuningSettings.Field findCpuTuningField", 1)[0]
        self.assertIn('if ("per_game_profile".equals(field.key))', cpu_controls)
        self.assertIn("continue;", cpu_controls)
        self.assertIn("enableCpuGameOverrideIfNeeded(titleId)", cpu_controls)
        self.assertNotIn("cpu_tuning_use_game_profile", SETTINGS)
        self.assertNotIn("cpu_tuning_use_game_profile", STRINGS)
        self.assertNotIn("cpu_tuning_use_game_profile", STRINGS_RU)

    def test_overlay_editor_renders_the_actual_gameplay_overlay(self):
        self.assertIn("static final class DualShockOverlayView extends View", MAIN)
        self.assertIn("new MainActivity.DualShockOverlayView", EDITOR)
        self.assertIn("gameplayPreview.controlBounds(state.element)", EDITOR)
        self.assertIn("gameplayPreview.previewLayout(states)", EDITOR)
        self.assertNotIn("canvas.drawColor(0xff080d14)", EDITOR)
        self.assertNotIn("drawDpadButton", EDITOR)
        self.assertNotIn("drawFaceButtons", EDITOR)


if __name__ == "__main__":
    unittest.main()
