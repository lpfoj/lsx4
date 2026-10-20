from pathlib import Path
import re
import os
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
MAIN_ACTIVITY = ROOT / "android-app/app/src/main/java/app/lsx4/android/MainActivity.java"


class AndroidGraphicsProfileRollbackContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.source = MAIN_ACTIVITY.read_text(encoding="utf-8")

    def test_game_start_seeds_missing_measured_defaults(self) -> None:
        self.assertIn(
            "seedGraphicsDefaults(appSettings)",
            self.source,
        )

    def test_profile_is_persisted_before_debug_override_and_marker_materialization(self) -> None:
        self.assertIn("editor.commit()", self.source)
        baseline = self.source.index("seedGraphicsDefaults(appSettings")
        override = self.source.index("applyDebugGraphicsProfileOverride(getIntent()")
        materialize = self.source.index("materializeGraphicsEffectSettings(")
        self.assertLess(baseline, override)
        self.assertLess(override, materialize)

    def test_profiling_override_is_debug_only_explicit_and_persisted(self) -> None:
        match = re.search(
            r"private void applyDebugGraphicsProfileOverride\(.*?\n    \}",
            self.source,
            flags=re.DOTALL,
        )
        self.assertIsNotNone(match)
        body = match.group(0)
        self.assertIn("!BuildConfig.DEBUG", body)
        self.assertIn("EXTRA_PROFILE_GRAPHICS_OVERRIDE", body)
        self.assertIn("intent.hasExtra", body)
        self.assertIn("editor.commit()", body)
        self.assertIn("Debug graphics profile override applied", body)

    def test_profiling_override_covers_every_effect(self) -> None:
        extras = (
            "profile_disable_dynamic_shadows",
            "profile_disable_ssao",
            "profile_disable_motion_blur",
            "profile_disable_depth_of_field",
            "profile_disable_anti_aliasing",
            "profile_disable_chromatic_aberration",
        )
        for extra in extras:
            self.assertIn(extra, self.source)


class GraphicsDefaultsExecutionTest(unittest.TestCase):
    """Compile the production seed method, not a Python/Java reimplementation."""
    KEYS = (
        "K_DISABLE_DYNAMIC_SHADOWS", "K_DISABLE_SSAO", "K_DISABLE_MOTION_BLUR",
        "K_DISABLE_DEPTH_OF_FIELD", "K_DISABLE_ANTI_ALIASING", "K_DISABLE_CHROMATIC_ABERRATION",
    )

    @classmethod
    def setUpClass(cls):
        executable = ".exe" if os.name == "nt" else ""
        homes = [Path(os.environ.get("JAVA_HOME", "__unset_java_home__")),
                 Path(os.environ.get("ProgramFiles", "C:/Program Files"))
                 / "Android/Android Studio/jbr"]
        javac = shutil.which("javac") or next(
            (str(h / "bin" / ("javac" + executable)) for h in homes
             if (h / "bin" / ("javac" + executable)).is_file()), None)
        if not javac:
            raise unittest.SkipTest("JDK required for production graphics-default execution")
        cls.java = str(Path(javac).with_name("java" + executable))
        if not Path(cls.java).is_file():
            raise unittest.SkipTest("Java runtime next to javac required")
        source = MAIN_ACTIVITY.read_text(encoding="utf-8")
        match = re.search(r"private void seedGraphicsDefaults\(.*?^    }", source,
                          re.DOTALL | re.MULTILINE)
        if match is None:
            raise AssertionError("Missing production seedGraphicsDefaults method")
        cls.production_method = match.group(0)
        settings = (MAIN_ACTIVITY.parent / "SettingsActivity.java").read_text(encoding="utf-8")
        constants = []
        for key in (*cls.KEYS, "K_RES_MODE"):
            declaration = re.search(r"(?:public )?static final String " + key
                                    + r'\s*=\s*"([^"]+)";', settings)
            if declaration is None:
                raise AssertionError("Missing real preference key " + key)
            constants.append(f'static final String {key} = "{declaration.group(1)}";')
        cls.scratch = tempfile.TemporaryDirectory(prefix="lsx4-graphics-defaults-")
        cls.addClassCleanup(cls.scratch.cleanup)
        cls.directory = Path(cls.scratch.name)
        harness = cls.HARNESS.replace("__METHOD__", cls.production_method).replace(
            "__CONSTANTS__", "\n".join(constants)).replace(
            "__KEYS__", ", ".join("SettingsActivity." + key for key in cls.KEYS))
        (cls.directory / "GraphicsDefaultsHarness.java").write_text(harness, encoding="utf-8")
        compiled = subprocess.run([javac, "-encoding", "UTF-8", "GraphicsDefaultsHarness.java"],
                                  cwd=cls.directory, capture_output=True, text=True, timeout=45)
        if compiled.returncode:
            raise AssertionError(compiled.stdout + compiled.stderr)

    def check_case(self, name):
        result = subprocess.run([self.java, "-cp", str(self.directory),
                                 "GraphicsDefaultsHarness", name],
                                capture_output=True, text=True, timeout=20)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("PASS " + name, result.stdout)

    def test_missing_preferences_seed_six_defaults_without_storing_resolution(self):
        self.check_case("missing")

    def test_every_explicit_false_survives_first_launch(self):
        self.check_case("explicit-false")

    def test_second_launch_preserves_edits_and_only_seeds_newly_absent_keys(self):
        self.check_case("relaunch")

    def test_existing_resolution_and_unrelated_settings_are_not_written(self):
        self.check_case("resolution")

    HARNESS = r"""
import java.util.*;
public final class GraphicsDefaultsHarness {
    static final String TAG = "test";
    // Deliberately differs from saved values: launch presentation is not persistence.
    int renderResolutionMode = 99;
    static final class SettingsActivity { __CONSTANTS__ }
    static final String[] KEYS = { __KEYS__ };
    static final class Log {
        static int e(String tag, String message) { return 0; }
        static int i(String tag, String message) { return 0; }
    }
    interface SharedPreferences {
        boolean contains(String key);
        Editor edit();
        interface Editor {
            Editor putBoolean(String key, boolean value);
            Editor putInt(String key, int value);
            boolean commit();
        }
    }
    static final class Prefs implements SharedPreferences {
        final Map<String, Object> saved = new LinkedHashMap<>();
        final List<Map<String, Object>> writes = new ArrayList<>();
        public boolean contains(String key) { return saved.containsKey(key); }
        public Editor edit() {
            return new Editor() {
                final Map<String, Object> delta = new LinkedHashMap<>();
                public Editor putBoolean(String key, boolean value) { delta.put(key, value); return this; }
                public Editor putInt(String key, int value) { delta.put(key, value); return this; }
                public boolean commit() {
                    writes.add(new LinkedHashMap<>(delta)); saved.putAll(delta); return true;
                }
            };
        }
    }
    __METHOD__
    static void require(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }
    public static void main(String[] args) {
        GraphicsDefaultsHarness activity = new GraphicsDefaultsHarness();
        String scenario = args[0];
        if (scenario.equals("missing")) {
            Prefs p = new Prefs();
            activity.seedGraphicsDefaults(null);
            activity.seedGraphicsDefaults(p);
            require(p.saved.size() == KEYS.length, "Only six missing effects may be materialized: " + p.saved);
            for (String key : KEYS) require(Boolean.TRUE.equals(p.saved.get(key)), "Missing default " + key);
            require(!p.saved.containsKey(SettingsActivity.K_RES_MODE), "Launch resolution became a saved override");
            require(p.writes.size() == 1, "Initial defaults must be one completed transaction");
        } else if (scenario.equals("explicit-false")) {
            // Check each flag independently so an omitted key cannot hide behind another.
            for (String preserved : KEYS) {
                Prefs p = new Prefs(); p.saved.put(preserved, false);
                activity.seedGraphicsDefaults(p);
                require(Boolean.FALSE.equals(p.saved.get(preserved)), "Overwrote explicit false: " + preserved);
                for (String key : KEYS) if (!key.equals(preserved))
                    require(Boolean.TRUE.equals(p.saved.get(key)), "Did not seed " + key);
                for (Map<String, Object> delta : p.writes)
                    require(!delta.containsKey(preserved), "Rewrote an existing preference: " + preserved);
            }
        } else if (scenario.equals("relaunch")) {
            Prefs p = new Prefs(); activity.seedGraphicsDefaults(p);
            for (int i = 0; i < KEYS.length; i++) p.saved.put(KEYS[i], (i & 1) == 0);
            Map<String, Object> edited = new LinkedHashMap<>(p.saved);
            p.writes.clear(); activity.seedGraphicsDefaults(p);
            require(p.saved.equals(edited), "Second launch reset user choices");
            require(p.writes.isEmpty(), "Second launch unnecessarily committed existing settings");
            p.saved.remove(KEYS[0]); activity.seedGraphicsDefaults(p);
            require(p.writes.size() == 1 && p.writes.get(0).size() == 1
                    && Boolean.TRUE.equals(p.writes.get(0).get(KEYS[0])), "Must seed only the newly absent key");
            for (int i = 1; i < KEYS.length; i++)
                require(p.saved.get(KEYS[i]).equals(edited.get(KEYS[i])), "Partial seed reset " + KEYS[i]);
        } else if (scenario.equals("resolution")) {
            Prefs p = new Prefs(); p.saved.put(SettingsActivity.K_RES_MODE, 7);
            p.saved.put("unrelated_setting", "keep");
            activity.seedGraphicsDefaults(p);
            require(Integer.valueOf(7).equals(p.saved.get(SettingsActivity.K_RES_MODE)), "Changed saved resolution");
            require("keep".equals(p.saved.get("unrelated_setting")), "Changed an unrelated setting");
            for (Map<String, Object> delta : p.writes) {
                require(!delta.containsKey(SettingsActivity.K_RES_MODE), "Rewrote saved resolution");
                require(!delta.containsKey("unrelated_setting"), "Rewrote unrelated preference");
            }
        } else throw new AssertionError("Unknown scenario " + scenario);
        System.out.println("PASS " + scenario);
    }
}
"""


if __name__ == "__main__":
    unittest.main()
