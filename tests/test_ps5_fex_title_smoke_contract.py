import os
import re
import shutil
import subprocess
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SCRIPT_PATH = ROOT / "tools" / "codex_ps5_fex_title_smoke.sh"
SCRIPT = SCRIPT_PATH.read_text(encoding="utf-8")


class Ps5FexTitleSmokeContractTests(unittest.TestCase):
    def test_only_the_physical_vivo_is_allowed_and_every_adb_call_is_guarded(self):
        self.assertIn("expected_device='10AG263F1B001HM'", SCRIPT)
        self.assertIn("device:vivo", SCRIPT)
        direct = [
            line.strip()
            for line in SCRIPT.splitlines()
            if re.match(r"^\s*adb(?:\s|$)", line)
        ]
        self.assertEqual(direct, ['adb -s "$device" "$@"'])
        wrapper = SCRIPT.split("adb_device() {", 1)[1].split("\n}", 1)[0]
        self.assertLess(
            wrapper.index("guard_device_action"), wrapper.index('adb -s "$device"')
        )
        self.assertNotIn("emulator-5554", SCRIPT)
        self.assertNotIn("kill-server", SCRIPT)

    def test_launches_a_ps5_directory_and_supports_both_storage_roots(self):
        for token in (
            '^PPSA[0-9]{5}$',
            'files/lsx4-home/games/$title',
            'files/lsx4-home/runtime-fs/user/app/$title',
            '--es game_platform ps5',
            '--es launch_game_path $game',
            'sce_sys/param.json',
        ):
            self.assertIn(token, SCRIPT)

    def test_requires_real_fex_evidence_without_claiming_playable(self):
        for token in (
            "managed_guest_cpu_backend",
            'value="1"',
            'backend.get("platform") == "ps5"',
            'backend.get("effective") == "fex-experimental"',
            "launch_attempted",
            "adapter_active",
            "fatal_pattern",
            '"playableClaim": False',
            "ps5-fex-active-needs-gameplay-route",
            "ps5-fex-launch-failed-or-incomplete",
        ):
            self.assertIn(token, SCRIPT)

    def test_shell_syntax(self):
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
            [str(bash), "-n", str(SCRIPT_PATH)], capture_output=True, text=True
        )
        self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main()
