import os
import re
import shutil
import subprocess
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
GRADLE = (ROOT / "android-app/app/build.gradle").read_text(encoding="utf-8")
RUNTIME = (ROOT / "src/executor/ps1_classic_runtime.cpp").read_text(
    encoding="utf-8"
)
SMOKE_PATH = ROOT / "tools" / "codex_ps1_current_jit_smoke.sh"
SMOKE = SMOKE_PATH.read_text(encoding="utf-8")


class Ps1ClassicRuntimeContractTests(unittest.TestCase):
    def test_swanstation_build_is_pinned_to_api26_arm64(self):
        for token in (
            'swanstationCommit = "5430a4a53b89fa5827c97b84ada29d23317245bc"',
            'swanstationNdkVersion = "29.0.14206865"',
            'swanstationCmakeVersion = "3.22.1"',
            '"-DANDROID_ABI=arm64-v8a"',
            '"-DANDROID_PLATFORM=android-26"',
            '"-DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON"',
            '"--target", "swanstation_libretro"',
            "SOURCE_DATE_EPOCH: swanstationSourceDateEpoch",
            '"status", "--porcelain", "--untracked-files=normal"',
            "outputs.upToDateWhen { false }",
        ):
            self.assertIn(token, GRADLE)

    def test_runtime_staging_requires_and_renames_the_ps1_core(self):
        prepare = GRADLE.split(
            'def prepareRuntimeJniLibs = tasks.register("prepareRuntimeJniLibs")', 1
        )[1].split("def registerFexPackagingVerifier", 1)[0]
        self.assertRegex(
            prepare,
            r"dependsOn\([^)]*\bbuildSwanstationAndroid\b[^)]*\)",
        )
        self.assertIn("inputs.files(ps4Runtime, ps5Runtime, swanstationCore", prepare)
        self.assertIn("from(swanstationCore)", prepare)
        self.assertIn("rename { swanstationPackagedName }", prepare)
        self.assertIn("stagedPs1Core", prepare)

    def test_executor_freshness_gate_includes_ps1_runtime_sources(self):
        prepare = GRADLE.split(
            'def prepareRuntimeJniLibs = tasks.register("prepareRuntimeJniLibs")', 1
        )[1].split("def registerFexPackagingVerifier", 1)[0]
        for token in (
            'rootProject.file("../src/executor/ps1_classic_runtime.cpp")',
            'rootProject.file("../src/executor/ps1_classic_runtime.h")',
            "ps4RuntimeSources",
            "newestPs4Source",
            "ps4Runtime.lastModified() < newestPs4Source.lastModified()",
        ):
            self.assertIn(token, prepare)

    def test_final_apk_has_a_content_gate_for_the_core(self):
        for token in (
            'expectedEntry = "lib/arm64-v8a/${swanstationPackagedName}"',
            "ZipFile archive = new ZipFile(apk)",
            "archive.getEntry(expectedEntry)",
            "strippedPs1Core",
            "packagedSha256 != expectedSha256",
            'task.name == "packageDebug"',
            'verifyPs1CoreInApks("debug")',
        ):
            self.assertIn(token, GRADLE)

    def test_missing_core_fails_before_runtime_becomes_active(self):
        launch = RUNTIME.split("int Launch(", 1)[1].split("void Stop()", 1)[0]
        preflight = launch.index("if (!LoadCore(g_runtime.api))")
        active = launch.index("g_runtime.active.store(true")
        thread = launch.index("g_runtime.thread = std::thread(Run)")
        self.assertLess(preflight, active)
        self.assertLess(active, thread)
        self.assertIn("return -3;", launch)

    def test_failed_symbol_preflight_closes_the_dso(self):
        loader = RUNTIME.split("bool LoadCore(CoreApi& api)", 1)[1].split(
            "void CloseCore", 1
        )[0]
        self.assertIn("if (!loaded)", loader)
        self.assertIn("dlclose(api.module);", loader)
        self.assertIn("api = {};", loader)

    def test_current_jit_smoke_is_vivo_guarded_and_restores_fex(self):
        self.assertIn("expected_device='10AG263F1B001HM'", SMOKE)
        direct = [
            line.strip()
            for line in SMOKE.splitlines()
            if re.match(r"^\s*adb(?:\s|$)", line)
        ]
        self.assertEqual(direct, ['adb -s "$device" "$@"'])
        wrapper = SMOKE.split("adb_device() {", 1)[1].split("\n}", 1)[0]
        self.assertLess(
            wrapper.index("guard_device_action"), wrapper.index('adb -s "$device"')
        )
        for token in (
            "trap restore_fex_backend EXIT",
            's/value=\\"1\\"/value=\\"0\\"/',
            's/value=\\"0\\"/value=\\"1\\"/',
            "PS1_CURRENT_SMOKE_BACKEND_RESTORED value=1",
        ):
            self.assertIn(token, SMOKE)
        self.assertNotIn("emulator-5554", SMOKE)
        self.assertNotIn("kill-server", SMOKE)

    def test_current_jit_smoke_requires_real_ps1_progress(self):
        for token in (
            'requested == "current-aarch64-jit"',
            'effective == "current-aarch64-jit"',
            '"[PS1_DIRECT] launch app0="',
            '"[PS1_DIRECT] core=SwanStation"',
            '"AArch64 JIT launch result: 0"',
            '"[PS1_DIRECT] AAudio start result=0"',
            "len(samples) >= 4",
            "fps_median >= 58.0",
            "present_delta >= 180",
            "core_median < budget_median",
        ):
            self.assertIn(token, SMOKE)

    def test_current_jit_smoke_shell_syntax(self):
        if os.name == "nt":
            bash = Path(os.environ.get("ProgramFiles", "")) / "Git" / "bin" / "bash.exe"
            if not bash.is_file():
                self.skipTest("Git Bash is unavailable")
        else:
            found = shutil.which("bash")
            if not found:
                self.skipTest("bash is unavailable")
            bash = Path(found)
        result = subprocess.run(
            [str(bash), "-n", str(SMOKE_PATH)], capture_output=True, text=True
        )
        self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main()
