import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
CORE = (
    ROOT / "src" / "executor" / "dynamic_translation" /
    "retiring_execution_core.cpp"
).read_text(encoding="utf-8")
GRADLE = (ROOT / "android-app" / "app" / "build.gradle").read_text(
    encoding="utf-8"
)


class Ps5CurrentHleGprCoherencyContractTests(unittest.TestCase):
    def test_ps5_current_jit_keeps_gprs_memory_authoritative(self):
        match = re.search(
            r"bool generic_gpr_cache_eligible\s*=.*?"
            r"#if defined\(LSX4_PS5_DESKTOP_PATH\)(.*?)#endif",
            CORE,
            re.DOTALL,
        )
        self.assertIsNotNone(match, "PS5 GPR coherency guard is missing")
        guard = match.group(1)
        self.assertIn("generic_gpr_cache_eligible = false;", guard)
        code = re.sub(r"//[^\n]*", "", guard)
        self.assertRegex(code, r"^\s*generic_gpr_cache_eligible\s*=\s*false;\s*$")

    def test_guard_is_ps5_only_and_does_not_disable_android_ps4_fex(self):
        marker = "generic_gpr_cache_eligible = false;"
        guard_match = re.search(
            r"#if defined\(LSX4_PS5_DESKTOP_PATH\)(?:\s|//[^\n]*\n)*"
            r"generic_gpr_cache_eligible\s*=\s*false;\s*#endif",
            CORE,
            re.DOTALL,
        )
        self.assertIsNotNone(guard_match)
        guarded = guard_match.group(0)
        self.assertIn(marker, guarded)
        self.assertNotIn("LSX4_ANDROID", guarded)
        self.assertNotIn("LSX4_FEX", guarded)

    def test_apk_freshness_tracks_the_shared_jit_source_for_ps5(self):
        ps5_sources = GRADLE.split("List<File> ps5RuntimeSources = [", 1)[1]
        ps5_sources = ps5_sources.split("]", 1)[0]
        self.assertIn(
            "src/executor/dynamic_translation/retiring_execution_core.cpp",
            ps5_sources,
        )
        self.assertIn(
            "src/executor/dynamic_translation/retiring_execution_core.h",
            ps5_sources,
        )


if __name__ == "__main__":
    unittest.main()
