import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (
    ROOT
    / "android-app/app/src/main/java/app/lsx4/android/RemoteDiagnostics.java"
).read_text(encoding="utf-8")


class RemoteDiagnosticsDefaultOffContractTests(unittest.TestCase):
    def test_collector_is_explicit_opt_in_even_in_debug_builds(self):
        enabled = SOURCE.split("static boolean isEnabled(Context context)", 1)[1]
        enabled = enabled.split("static void initialize", 1)[0]
        self.assertIn("getBoolean(\n                PREF_ENABLED, false)", enabled)
        self.assertNotIn("BuildConfig.DEBUG", enabled)

    def test_manual_upload_can_still_enable_the_collector(self):
        request = SOURCE.split("static void requestImmediateUpload", 1)[1]
        request = request.split("private static final class State", 1)[0]
        self.assertIn("setEnabled(context, true)", request)


if __name__ == "__main__":
    unittest.main()
