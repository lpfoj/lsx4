import unittest
from pathlib import Path
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
LAUNCHER = ROOT / "android-app/app/src/main/java/app/lsx4/android/LauncherUi.java"
RES = ROOT / "android-app/app/src/main/res"


class LibrarySortSearchContract(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.source = LAUNCHER.read_text(encoding="utf-8")

    def test_sort_button_is_left_of_add_and_long_click_consumes_event(self):
        self.assertLess(self.source.index("appBar.addView(sort,"), self.source.index("appBar.addView(add,"))
        callback = self.source.split("sort.setOnLongClickListener", 1)[1].split("});", 1)[0]
        self.assertIn("toggleSearch();", callback)
        self.assertIn("return true;", callback)

    def test_sort_and_search_do_not_rewrite_manual_order_or_rescan(self):
        actions = self.source.split("private void showSortMenu", 1)[1].split("public View createView", 1)[0]
        self.assertNotIn("persistGameOrder", actions)
        self.assertNotIn("scanInstalledGames", actions)
        self.assertNotIn("renderGameList(", actions)
        self.assertIn("renderVisibleGames();", actions)

    def test_search_is_hidden_by_default_and_clearing_restores_all_titles(self):
        self.assertIn("searchRow.setVisibility(View.GONE)", self.source)
        self.assertIn("close.setOnClickListener(view -> hideSearch())", self.source)
        close = self.source.split("private void hideSearch()", 1)[1].split("private void scrollGamesToTop", 1)[0]
        self.assertIn('searchInput.setText("")', close)
        self.assertIn("hideSearchKeyboard();", close)
        self.assertIn("renderVisibleGames();", close)

    def test_english_is_a_field_hint_not_a_global_keyboard_change(self):
        self.assertIn("setImeHintLocales(new LocaleList(Locale.ENGLISH))", self.source)
        self.assertIn("EditorInfo.IME_FLAG_FORCE_ASCII", self.source)
        self.assertNotIn("Settings.Secure.put", self.source)

    def test_search_and_sort_do_not_remove_games_from_profile_catalog(self):
        render = self.source.split("private void renderGameList", 1)[1].split("private View gameRow", 1)[0]
        self.assertLess(render.index("replaceInstalledTitles"), render.index("GameLibraryView.visible"))
        self.assertIn("latestGames = orderedGames", render)
        self.assertNotIn("latestGames = visibleGames", render)

    def test_new_labels_exist_in_both_english_and_russian(self):
        names = {"library_sort_action", "library_sort_manual", "library_sort_reverse",
                 "library_sort_playable", "library_sort_unplayable", "library_search_hint",
                 "library_search_hide", "library_search_empty", "library_move_manual_only"}
        for locale in ("values", "values-ru"):
            strings = ET.parse(RES / locale / "strings.xml").getroot().findall("string")
            self.assertTrue(names.issubset({node.attrib["name"] for node in strings}))
            self.assertEqual(len(strings), len({node.attrib["name"] for node in strings}))


if __name__ == "__main__":
    unittest.main()
