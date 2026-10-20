#!/usr/bin/env python3
"""Static contracts for the persistent touch-overlay editor and phone triggers."""

from pathlib import Path
import re
import unittest
import xml.etree.ElementTree as ET


ROOT = Path(__file__).resolve().parents[1]
JAVA = ROOT / "android-app/app/src/main/java/app/lsx4/android"


class OverlayEditorContract(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.store = (JAVA / "OverlayLayoutStore.java").read_text(encoding="utf-8")
        cls.editor = (JAVA / "OverlayEditorActivity.java").read_text(encoding="utf-8")
        cls.mapper = (JAVA / "HardwareTriggerMapper.java").read_text(encoding="utf-8")
        cls.settings = (JAVA / "SettingsActivity.java").read_text(encoding="utf-8")
        cls.main = (JAVA / "MainActivity.java").read_text(encoding="utf-8")
        cls.manifest = (ROOT / "android-app/app/src/main/AndroidManifest.xml").read_text(
            encoding="utf-8"
        )

    def test_editor_resolves_resources_using_the_selected_app_locale(self):

        attach = re.search(
            r"@Override\s+protected void attachBaseContext\(Context base\)\s*\{([^}]+)\}",
            self.editor,
        )
        self.assertIsNotNone(attach)
        self.assertIn("super.attachBaseContext(AppLocale.wrap(base));", attach.group(1))
        locale = (JAVA / "AppLocale.java").read_text(encoding="utf-8")
        self.assertIn('ProcessSafePreferences.get(base)', locale)
        self.assertIn('.getString(KEY, "en")', locale)
        self.assertIn("configuration.setLocales(new LocaleList(locale))", locale)
        self.assertIn("base.createConfigurationContext(configuration)", locale)

    def test_editor_labels_are_available_in_every_supported_app_locale(self):
        required = set(re.findall(r"R\.string\.(overlay_editor_\w+)", self.editor))
        self.assertTrue(required)
        resources = ROOT / "android-app/app/src/main/res"
        for directory in ("values", "values-es", "values-fr", "values-de", "values-ru", "values-zh-rCN"):
            with self.subTest(locale=directory):
                strings = {}
                for path in (resources / directory).glob("*.xml"):
                    for entry in ET.parse(path).getroot().findall("string"):
                        strings[entry.get("name")] = "".join(entry.itertext()).strip()
                self.assertFalse(required - strings.keys())
                self.assertTrue(all(strings[key] for key in required))

    def test_optional_buttons_are_hidden_by_default(self):
        for name in ("SHARE", "PS", "TOUCHPAD"):
            self.assertRegex(
                self.store,
                rf"static final Element {name} = new Element\([^;]+, false\);",
            )

    def test_layout_is_normalized_persistent_and_resolution_independent(self):
        self.assertIn("putFloat(xKey(state.element)", self.store)
        self.assertIn("putFloat(yKey(state.element)", self.store)
        self.assertIn("putFloat(scaleKey(state.element)", self.store)
        self.assertIn("state.offsetX * hostWidth", self.store)
        self.assertIn("state.offsetY * hostHeight", self.store)
        self.assertIn("RectF... rects", self.store)

    def test_editor_supports_drag_pinch_hide_restore_and_reset(self):
        self.assertIn("ScaleGestureDetector", self.editor)
        self.assertIn("selected.offsetX +=", self.editor)
        self.assertIn("selected.offsetY +=", self.editor)
        self.assertIn("void hideSelected()", self.editor)
        self.assertIn("void restore(OverlayLayoutStore.Element element)", self.editor)
        self.assertIn("OverlayLayoutStore.reset(preferences)", self.editor)
        self.assertIn("action.setOnClickListener(listener)", self.editor)

    def test_system_bars_are_hidden_only_after_content_view_is_attached(self):
        content = self.editor.index("setContentView(root);")
        schedule = self.editor.index("hideSystemBarsWhenAttached(root);")
        lookup = self.editor.index("root.getWindowInsetsController()")
        self.assertLess(content, schedule)
        self.assertLess(schedule, lookup)
        self.assertIn("root.post(() ->", self.editor)
        self.assertNotIn("getWindow().getInsetsController()", self.editor)

    def test_settings_expose_editor_and_three_localized_visibility_switches(self):
        self.assertIn("new Intent(this, OverlayEditorActivity.class)", self.settings)
        for key in (
            "K_OVERLAY_SHOW_SHARE",
            "K_OVERLAY_SHOW_PS",
            "K_OVERLAY_SHOW_TOUCHPAD",
        ):
            self.assertIn(key, self.settings)
        for resource in (
            "R.string.controls_show_share",
            "R.string.controls_show_ps",
            "R.string.controls_show_touchpad",
        ):
            self.assertIn(resource, self.settings)

    def test_game_overlay_reloads_and_uses_layout_for_draw_and_hit_test(self):
        self.assertIn("dualShockOverlay.reloadLayout()", self.main)
        self.assertIn("OverlayLayoutStore.isPreferenceKey(key)", self.main)
        self.assertIn("applyLayoutTransform(OverlayLayoutStore.DPAD", self.main)
        self.assertIn("isVisible(OverlayLayoutStore.SHARE)", self.main)
        self.assertIn("mask |= PAD_SHARE", self.main)
        self.assertIn("mask |= PAD_PS", self.main)

    def test_editor_is_available_inside_game_process(self):
        activity = re.search(
            r'<activity\s+android:name="app\.lsx4\.android\.OverlayEditorActivity"(.+?)/>',
            self.manifest,
            flags=re.S,
        )
        self.assertIsNotNone(activity)
        self.assertIn('android:process=":game"', activity.group(0))
        self.assertIn('android:screenOrientation="landscape"', activity.group(0))

    def test_trigger_capture_is_portable_and_runs_before_normal_pad_mapping(self):
        lowered = self.mapper.lower()
        for forbidden in ("build.model", "manufacturer", "vendorid", "productid", "deviceid"):
            self.assertNotIn(forbidden, lowered)
        self.assertIn("event.getKeyCode()", self.mapper)
        self.assertIn("event.getScanCode()", self.mapper)
        mapping = self.main.index("HardwareTriggerMapper.map(liveSettings, event)")
        normal = self.main.index("handlePadKey(event.getKeyCode(), pressed, event)")
        self.assertLess(mapping, normal)


if __name__ == "__main__":
    unittest.main()
