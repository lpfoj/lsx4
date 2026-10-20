import os
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SCRIPT_PATH = ROOT / "tools" / "codex_fex_title_smoke.sh"
SCRIPT = SCRIPT_PATH.read_text(encoding="utf-8")


class FexTitleSmokeContractTests(unittest.TestCase):
    def run_activation_evidence(
        self, log, launch_path, stage, effective="fex-experimental", runtime_log=None
    ):
        evidence_code = SCRIPT.split(
            'live_samples = re.findall(r"\\[EXECUTOR_LIVE_FPS\\].*", log)', 1
        )[1].split("fatal = fatal_pattern.search(log)", 1)[0]
        evidence_code = (
            'live_samples = re.findall(r"\\[EXECUTOR_LIVE_FPS\\].*", log)'
            + evidence_code
        )
        evidence_code += "\nprocess_alive = " + SCRIPT.split(
            "process_alive = ", 1
        )[1].split("launchable = (", 1)[0]
        with tempfile.TemporaryDirectory() as temp_dir:
            output = Path(temp_dir)
            (output / "guest-cpu-backend-status.json").write_text(
                '{"effective":"' + effective + '"}', encoding="utf-8"
            )
            (output / "last-launch-stage.json").write_text(
                '{"path":"' + launch_path + '","stage":"' + stage + '"}',
                encoding="utf-8",
            )
            namespace = {
                "game_path": (
                    "/data/user/0/app.lsx4.android/files/lsx4-home/"
                    "runtime-fs/user/app/CUSA00920/eboot.bin"
                ),
                "log": log,
                "runtime_log": log if runtime_log is None else runtime_log,
                "output": output,
                "pid_text": "12345",
                "read_json": lambda path: __import__("json").loads(
                    path.read_text(encoding="utf-8")
                ),
                "re": re,
            }
            exec(evidence_code, namespace)
            return namespace

    def test_physical_device_and_every_adb_call_is_guarded(self):
        self.assertIn("expected_device='10AG263F1B001HM'", SCRIPT)
        self.assertIn("device:vivo", SCRIPT)
        direct = [
            line.strip()
            for line in SCRIPT.splitlines()
            if re.match(r"^\s*adb(?:\s|$)", line)
        ]
        self.assertEqual(direct, ['adb -s "$device" "$@"'])
        wrapper = SCRIPT.split("adb_device() {", 1)[1].split("\n}", 1)[0]
        self.assertLess(wrapper.index("guard_device_action"), wrapper.index('adb -s "$device"'))
        self.assertNotIn("emulator-5554", SCRIPT)
        self.assertNotIn("kill-server", SCRIPT)

    def test_smoke_is_fex_only_and_does_not_claim_playable(self):
        for token in (
            "managed_guest_cpu_backend",
            'value="1"',
            "[EXECUTOR_FEX_ACTIVATION] stage=commit result=ok",
            '"playableClaim": False',
            "launchable-needs-gameplay-route",
            "fex-launch-failed-or-incomplete",
            "last-fex-entry-run.json",
            "last-launch-stage.json",
            "logcat-boot.txt",
            "logcat-runtime.txt",
            "run-as $package rm -f",
        ):
            self.assertIn(token, SCRIPT)

    def test_second_button_is_allowlisted_and_supports_cutscene_skip(self):
        self.assertIn('readonly second_button="${5:-cross}"', SCRIPT)
        self.assertIn('[[ "$second_button" == cross || "$second_button" == circle ]]', SCRIPT)
        self.assertIn("reason=invalid-second-button", SCRIPT)
        self.assertIn("input swipe 2903 1231 2903 1231 300", SCRIPT)
        self.assertIn("input swipe 3050 1095 3050 1095 900", SCRIPT)
        self.assertIn('"secondButton": second_button', SCRIPT)
        self.assertNotIn("input keyevent", SCRIPT)

    def test_launchability_requires_renderer_evidence_and_rejects_known_fex_failures(self):
        self.assertIn(
            "renderer_active = bool(live_samples) and (draw_positive or present_positive)",
            SCRIPT,
        )
        self.assertIn("and renderer_active", SCRIPT)
        for token in (
            "fex_mapped_entry_failed",
            "EXECUTOR_FEX_ENTRY_RUN",
            "EXECUTOR_FEX_signal",
            "EXECUTOR_FEX_HLE_MANIFEST_LATE_REJECT",
            "EXECUTOR_MODULE_START_FEX",
        ):
            self.assertIn(token, SCRIPT)

    def test_evicted_activation_marker_has_strict_durable_live_fallback(self):
        for token in (
            'backend_status.get("effective") == "fex-experimental"',
            'normalized_launch_path == normalized_game_path',
            'launch_stage_name == "build_module_status_begin"',
            "recent_live_samples = metric_live_samples[-3:]",
            'runtime_live_samples = re.findall(r"\\[EXECUTOR_LIVE_FPS\\].*", runtime_log)',
            'sample["submitDoneRate"] > 0.0',
            'newer["frames"] > older["frames"]',
            'newer["builderTotal"] > older["builderTotal"]',
            "activation_committed or activation_inferred",
        ):
            self.assertIn(token, SCRIPT)
        fallback = SCRIPT.split("activation_inferred = (", 1)[1].split("\n)", 1)[0]
        for token in (
            "launch_stage_ready",
            "backend_status_fex",
            "process_alive",
            "recent_live_progress",
        ):
            self.assertIn(token, fallback)

    def test_build_module_status_begin_is_a_completed_load_checkpoint_not_a_hang(self):
        self.assertIn(
            "launch_stage_title_bound and launch_stage_name == \"build_module_status_begin\"",
            SCRIPT,
        )
        self.assertIn("launch_stage_ready", SCRIPT)
        self.assertNotRegex(
            SCRIPT,
            r"launch_stage_name\s*==\s*[\"']build_module_status_begin[\"']\s*\n?\s*and\s+.*hang",
        )

    def test_valid_durable_evidence_replaces_evicted_activation_marker(self):
        log = "\n".join(
            "[EXECUTOR_LIVE_FPS] frames={} submitDoneRate={} builderTotal={}".format(
                frames, rate, builder
            )
            for frames, rate, builder in (
                (3670, 61.0, 49664),
                (3730, 59.0, 50432),
                (3789, 60.0, 51456),
            )
        )
        evidence = self.run_activation_evidence(
            log,
            "/data/user/0/app.lsx4.android/files/lsx4-home/"
            "runtime-fs/user/app/CUSA00920/eboot.bin",
            "build_module_status_begin",
        )
        self.assertFalse(evidence["activation_committed"])
        self.assertTrue(evidence["launch_stage_ready"])
        self.assertTrue(evidence["recent_live_progress"])
        self.assertTrue(evidence["activation_inferred"])
        self.assertTrue(evidence["activation_evidence"])

    def test_durable_fallback_rejects_wrong_title_or_stalled_recent_frames(self):
        progressing = "\n".join(
            f"[EXECUTOR_LIVE_FPS] frames={value} submitDoneRate=60.0 builderTotal={value * 10}"
            for value in (100, 101, 102)
        )
        wrong_title = self.run_activation_evidence(
            progressing,
            "/data/user/0/app.lsx4.android/files/lsx4-home/"
            "runtime-fs/user/app/CUSA00000/eboot.bin",
            "build_module_status_begin",
        )
        self.assertFalse(wrong_title["launch_stage_title_bound"])
        self.assertFalse(wrong_title["activation_inferred"])

        stalled = self.run_activation_evidence(
            "\n".join(
                "[EXECUTOR_LIVE_FPS] frames=100 submitDoneRate=60.0 builderTotal=1000"
                for _ in range(3)
            ),
            "/data/user/0/app.lsx4.android/files/lsx4-home/"
            "runtime-fs/user/app/CUSA00920/eboot.bin",
            "build_module_status_begin",
        )
        self.assertFalse(stalled["recent_live_progress"])
        self.assertFalse(stalled["activation_inferred"])

    def test_boot_log_samples_do_not_pad_the_three_recent_runtime_samples(self):
        lines = [
            f"[EXECUTOR_LIVE_FPS] frames={value} submitDoneRate=60.0 builderTotal={value * 10}"
            for value in (100, 101, 102)
        ]
        evidence = self.run_activation_evidence(
            "\n".join(lines),
            "/data/user/0/app.lsx4.android/files/lsx4-home/"
            "runtime-fs/user/app/CUSA00920/eboot.bin",
            "build_module_status_begin",
            runtime_log="\n".join(lines[-2:]),
        )
        self.assertEqual(len(evidence["metric_live_samples"]), 2)
        self.assertFalse(evidence["recent_live_progress"])
        self.assertFalse(evidence["activation_inferred"])

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
        result = subprocess.run([str(bash), "-n", str(SCRIPT_PATH)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main()
