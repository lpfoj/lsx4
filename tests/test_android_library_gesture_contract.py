import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
JAVA = ROOT / "android-app/app/src/main/java/app/lsx4/android"
RES = ROOT / "android-app/app/src/main/res"


class AndroidLibraryGestureContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.launcher = (JAVA / "LauncherUi.java").read_text(encoding="utf-8")
        cls.hold_action = (JAVA / "GameHoldAction.java").read_text(encoding="utf-8")
        cls.locale_files = sorted(RES.glob("values*/strings.xml"))

    def test_hold_thresholds_are_exactly_two_three_four_five_seconds(self):
        for name, millis in (
            ("SELECT_MS", "2_000L"),
            ("CLEAR_SELECTION_MS", "3_000L"),
            ("MARK_UNSUPPORTED_MS", "4_000L"),
            ("MARK_SUPPORTED_MS", "5_000L"),
        ):
            self.assertIn(f"{name} = {millis}", self.hold_action)

    def test_item_accessibility_contains_no_obsolete_gesture_instructions(self):
        self.assertGreaterEqual(len(self.locale_files), 6)
        for path in self.locale_files:
            text = path.read_text(encoding="utf-8")
            match = re.search(
                r'<string name="game_item_accessibility">(.*?)</string>', text
            )
            self.assertIsNotNone(match, path)
            value = match.group(1).lower()
            self.assertEqual(1, value.count("%1$s"), path)
            self.assertEqual(1, value.count("%2$s"), path)
            self.assertEqual(1, value.count("%3$s"), path)
            for obsolete in ("double tap", "двойное", "8 секунд", "8 seconds", "8 sekunden"):
                self.assertNotIn(obsolete, value, path)

    def test_selected_game_can_move_by_drop_or_destination_tap(self):
        self.assertIn("moveSelectedGameTo(game);", self.launcher)
        self.assertIn("DragEvent.ACTION_DROP", self.launcher)
        self.assertIn("moveSelectedGameTo(target);", self.launcher)
        self.assertNotIn("setOnDoubleClickListener", self.launcher)

    def test_trash_requires_explicit_confirmation(self):
        self.assertIn(
            "removeAction.setOnClickListener(view -> confirmRemoveSelectedGame())",
            self.launcher,
        )
        self.assertIn("new AlertDialog.Builder(activity)", self.launcher)
        self.assertIn(".setNegativeButton(android.R.string.cancel, null)", self.launcher)
        self.assertIn("(dialog, which) -> removeSelectedGame()", self.launcher)


if __name__ == "__main__":
    unittest.main()
