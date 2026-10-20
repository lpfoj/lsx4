from html.parser import HTMLParser
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
REPORT = ROOT / "docs" / "BLOODBORNE_FRAME_REPORT_2026-08-11.html"
DOCS = ROOT / "docs"
CANONICAL = DOCS / "PERFORMANCE_TEST_PREFLIGHT.md"


class _IdCollector(HTMLParser):
    def __init__(self):
        super().__init__()
        self.ids = []

    def handle_starttag(self, _tag, attrs):
        attributes = dict(attrs)
        if "id" in attributes:
            self.ids.append(attributes["id"])


class PerformancePreflightReportContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.html = REPORT.read_text(encoding="utf-8")

    def test_preflight_is_visible_in_playable_and_performance_panels(self):
        playable = self.html.index('id="panel-playable"')
        playable_notice = self.html.index('id="playable-test-preflight"')
        playable_content = self.html.index('id="panel-playable-model"', playable_notice)
        self.assertLess(playable, playable_notice)
        self.assertLess(playable_notice, playable_content)

        performance = self.html.index('id="panel-performance"')
        performance_notice = self.html.index('id="performance-test-preflight"')
        performance_content = self.html.index(
            'id="panel-performance-model"', performance_notice
        )
        self.assertLess(performance, performance_notice)
        self.assertLess(performance_notice, performance_content)

    def test_notice_records_command_and_invalidates_dirty_measurements(self):
        for required in (
            "scripts/preflight-vivo-performance-test.ps1",
            "process-tracker",
            "1605",
            "11 → 29 FPS",
            "недействительным",
        ):
            self.assertIn(required, self.html)

    def test_report_ids_remain_unique(self):
        parser = _IdCollector()
        parser.feed(self.html)
        duplicates = sorted({item for item in parser.ids if parser.ids.count(item) > 1})
        self.assertEqual([], duplicates)

    def test_every_markdown_document_carries_the_preflight_gate(self):
        missing = []
        for document in sorted(DOCS.glob("*.md")):
            if document == CANONICAL:
                continue
            opening = "\n".join(document.read_text(encoding="utf-8").splitlines()[:10])
            if (
                "scripts/preflight-vivo-performance-test.ps1" not in opening
                or "PERFORMANCE_TEST_PREFLIGHT.md" not in opening
                or "invalid" not in opening
            ):
                missing.append(document.name)
        self.assertEqual([], missing)

    def test_canonical_preflight_records_the_proven_ab(self):
        canonical = CANONICAL.read_text(encoding="utf-8")
        for required in (
            "1,605 (`1605`)",
            "400–600% system CPU",
            "1.2 GiB free memory",
            "7.1 GiB swap",
            "11 → 29 FPS",
            "no emulator code change",
            "no emulator setting change",
        ):
            self.assertIn(required, canonical)


if __name__ == "__main__":
    unittest.main()
