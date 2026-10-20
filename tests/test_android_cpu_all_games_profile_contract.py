import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
TUNING = (
    ROOT / "android-app/app/src/main/java/app/lsx4/android/CpuTuningSettings.java"
).read_text(encoding="utf-8")
SETTINGS = (
    ROOT / "android-app/app/src/main/java/app/lsx4/android/SettingsActivity.java"
).read_text(encoding="utf-8")
ACTIVITY = (
    ROOT / "android-app/app/src/main/java/app/lsx4/android/MainActivity.java"
).read_text(encoding="utf-8")
STRINGS = (ROOT / "android-app/app/src/main/res/values/strings.xml").read_text(
    encoding="utf-8"
)
STRINGS_RU = (
    ROOT / "android-app/app/src/main/res/values-ru/strings.xml"
).read_text(encoding="utf-8")


class AndroidCpuAllGamesProfileContractTests(unittest.TestCase):
    def test_universal_profile_is_default_off_and_reversible(self):
        self.assertIn('K_ALL_GAMES_ENABLED = "cpu.all_games.enabled"', TUNING)
        self.assertIn(
            "preferences.getBoolean(K_ALL_GAMES_ENABLED, false)", TUNING
        )
        self.assertIn("setAllGamesProfileEnabled", TUNING)
        self.assertNotIn("remove(CpuTuningSettings.GAME_PREFIX", SETTINGS)

    def test_activation_snapshots_every_current_field_before_routing(self):
        activation = TUNING.split(
            "static void activateCurrentProfileForAllGames", 1
        )[1].split("static void setAllGamesProfileEnabled", 1)[0]
        self.assertLess(
            activation.index("selectedValues(preferences, titleId)"),
            activation.index("putBoolean(K_ALL_GAMES_ENABLED, true)"),
        )
        self.assertIn("for (Field field : FIELDS)", activation)
        self.assertIn('fieldKey(ALL_GAMES_PREFIX, "preset")', activation)
        self.assertIn("fieldKey(ALL_GAMES_PREFIX, field.key)", activation)
        self.assertEqual(activation.count("editor.commit()"), 1)

    def test_runtime_uses_universal_scope_ahead_of_per_game_scope(self):
        resolution = TUNING.split("private static Map<String, Integer> resolveValues", 1)[
            1
        ].split("static Map<String, Integer> effectiveRuntimeValues", 1)[0]
        self.assertIn(
            "boolean useAllGamesScope = allGamesProfileApplies(preferences, titleId)",
            resolution,
        )
        self.assertIn("boolean useGameScope = !useAllGamesScope", resolution)
        self.assertIn(
            "String scope = useAllGamesScope ? ALL_GAMES_PREFIX", resolution
        )
        self.assertIn(
            "if (useAllGamesScope || useGameScope)", resolution
        )

    def test_ui_checkbox_switches_to_editable_shared_profile(self):
        build = SETTINGS.split("private void buildCpuTuning", 1)[1].split(
            "private void buildManagedOptimizations", 1
        )[0]
        self.assertIn("R.string.cpu_tuning_apply_all_games", build)
        self.assertIn("activateCurrentProfileForAllGames", build)
        self.assertIn("showCpuAllGamesExceptions", build)
        self.assertIn("GameProfileSettings.installedGames", build)
        self.assertIn(
            "renderCpuTuningControls(controls, CpuTuningSettings.ALL_GAMES_PREFIX)",
            build,
        )
        self.assertIn("setAllGamesProfileEnabled(live, false)", build)
        self.assertIn('name="cpu_tuning_apply_all_games"', STRINGS)
        self.assertIn('name="cpu_tuning_apply_all_games"', STRINGS_RU)

    def test_shared_profile_crosses_launcher_game_process_boundary(self):
        java = ROOT / "android-app/app/src/main/java/app/lsx4/android"
        transport = (java / "ProcessSafePreferences.java").read_text(encoding="utf-8")
        provider = (java / "SettingsProvider.java").read_text(encoding="utf-8")
        scoped = (java / "GameProfileSettings.java").read_text(encoding="utf-8")
        prefs = SETTINGS.split("public static SharedPreferences prefsForGame(", 1)[1].split(
            "private SharedPreferences basePrefs()", 1
        )[0]
        self.assertIn("ProcessSafePreferences.get(context)", prefs)
        self.assertIn("GameProfileSettings.forGame(base, titleId)", prefs)
        self.assertIn("SettingsProvider.owner(storeContext)", transport)
        self.assertIn("SettingsProvider.remote(storeContext)", transport)


        key_routing = scoped.split("static boolean isIndividualKey(", 1)[1].split(
            "private String readKey(", 1
        )[0]
        self.assertNotIn("CpuTuningSettings.K_ALL_GAMES_ENABLED", key_routing)
        self.assertNotIn("CpuTuningSettings.K_ALL_GAMES_EXCEPTIONS", key_routing)
        self.assertNotIn("CpuTuningSettings.ALL_GAMES_PREFIX", key_routing)
        self.assertIn("return isIndividualKey(key) ? prefix + key : key", scoped)
        self.assertIn("Map<String, ?> source = base.getAll()", scoped)


        self.assertIn("PreferenceSnapshot.copy(preferences.getAll())", provider)
        self.assertIn("change.values.put(key, value == null ? null : new LinkedHashSet<>(value))",
                      transport)
        self.assertIn("change.values.putAll(unpack(extras.getBundle(VALUES)))", provider)
        self.assertIn("for (Map.Entry<String, Object> entry : change.values.entrySet())", provider)
        self.assertIn("if (change.clear) editor.clear()", provider)
        self.assertIn("editor.putStringSet(key, (Set<String>) value)", provider)
        self.assertNotIn("crossProcessSettingKeys", ACTIVITY)
        self.assertNotIn("applyCrossProcessSettingsSnapshot", ACTIVITY)

    def test_exception_bypasses_universal_profile_and_uses_game_profile(self):
        self.assertIn(
            'K_ALL_GAMES_EXCEPTIONS = "cpu.all_games.exceptions"', TUNING
        )
        applies = TUNING.split("static boolean allGamesProfileApplies", 1)[1].split(
            "static void activateCurrentProfileForAllGames", 1
        )[0]
        self.assertIn("!allGamesExceptions(preferences).contains(normalized)", applies)
        self.assertIn(
            "boolean useGameScope = !useAllGamesScope", TUNING
        )
        self.assertIn("setMultiChoiceItems(labels, checked", SETTINGS)
        self.assertIn("labels[index] = games.get(index).title", SETTINGS)

    def test_named_preset_keeps_title_game_db_boot_contracts(self):
        resolution = TUNING.split("private static Map<String, Integer> resolveValues", 1)[
            1
        ].split("static Map<String, Integer> effectiveRuntimeValues", 1)[0]
        self.assertIn("boolean customProfile = selectedPreset == PRESET_CUSTOM", resolution)
        self.assertLess(
            resolution.index("overlayExplicitValues(preferences, scope, values)"),
            resolution.rindex("applyGameDb(values, normalizeTitleId(titleId))"),
        )

    def test_individual_cpu_editor_auto_activates_without_redundant_checkbox(self):
        controls = SETTINGS.split("private void renderCpuTuningControls", 1)[1].split(
            "private CpuTuningSettings.Field findCpuTuningField", 1
        )[0]
        self.assertNotIn("R.string.cpu_tuning_use_game_profile", controls)
        self.assertIn("enableCpuGameOverrideIfNeeded(titleId)", controls)


if __name__ == "__main__":
    unittest.main()
