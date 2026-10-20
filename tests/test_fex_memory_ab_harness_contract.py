import json
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
HARNESS_PATH = ROOT / "tools" / "codex_bloodborne_fex_memory_ab.sh"
HARNESS = HARNESS_PATH.read_text(encoding="utf-8")


class FexMemoryAbHarnessContractTests(unittest.TestCase):
    @staticmethod
    def _inline_python_blocks():
        return re.findall(r"<<'PY'\n(.*?)\nPY\n", HARNESS, flags=re.DOTALL)

    def test_inline_python_gates_compile(self):
        blocks = self._inline_python_blocks()
        self.assertEqual(len(blocks), 3)
        for index, block in enumerate(blocks):
            compile(block, f"harness-inline-{index}.py", "exec")

    def test_backend_status_gate_accepts_only_the_exact_memory_contract(self):
        validator = self._inline_python_blocks()[0]

        def validate(status, flags, suffix, model):
            with tempfile.TemporaryDirectory() as temporary:
                status_path = Path(temporary) / "status.json"
                status_path.write_text(json.dumps(status), encoding="utf-8")
                return subprocess.run(
                    [
                        sys.executable,
                        "-",
                        str(status_path),
                        str(flags),
                        suffix,
                        model,
                    ],
                    input=validator,
                    text=True,
                    capture_output=True,
                    check=False,
                )

        baseline_model = (
            "FEX software TSO (hardware TSO when enabled by the host OS)"
        )
        baseline_suffix = "-lookup-throughput-sm-swtso"
        baseline = {
            "effective": "fex-2608-experimental",
            "supported": True,
            "fallbackReason": "none",
            "configureFlags": 0,
            "providedCapabilities": (1 << 2) | (1 << 12),
            "requiredCapabilities": 1 << 2,
            "forbiddenCapabilities": 0,
            "strongMemoryModel": baseline_model,
            "cacheNamespace": "fex-2608-ps4-v3-test" + baseline_suffix,
        }
        accepted = validate(baseline, 0, baseline_suffix, baseline_model)
        self.assertEqual(accepted.returncode, 0, accepted.stderr)
        self.assertIn("cacheSuffixRole=provenance-only", accepted.stdout)

        relaxed_model = "FEX software TSO (MemcpySetTSO relaxed A/B)"
        relaxed_suffix = "-lookup-throughput-ab-mtso0-sm-swtso"
        relaxed = {
            **baseline,
            "configureFlags": 2,
            "providedCapabilities": 1 << 12,
            "requiredCapabilities": 1 << 12,
            "forbiddenCapabilities": 1 << 2,
            "strongMemoryModel": relaxed_model,
            "cacheNamespace": "fex-2608-ps4-v3-test" + relaxed_suffix,
        }
        accepted = validate(relaxed, 2, relaxed_suffix, relaxed_model)
        self.assertEqual(accepted.returncode, 0, accepted.stderr)

        relaxed["providedCapabilities"] |= 1 << 2
        rejected = validate(relaxed, 2, relaxed_suffix, relaxed_model)
        self.assertNotEqual(rejected.returncode, 0)
        self.assertIn("relaxed phase still advertises/requires bit2", rejected.stderr)

    def test_uses_exact_vivo_and_never_targets_emulator(self):
        self.assertIn("expected_device='10AG263F1B001HM'", HARNESS)
        self.assertIn("if [[ \"$device\" != \"$expected_device\" ]]", HARNESS)
        self.assertNotIn("emulator-5554", HARNESS)
        self.assertNotIn("adb kill-server", HARNESS)
        self.assertNotIn("kill-server", HARNESS)

    def test_every_adb_action_is_agentchat_guarded(self):
        direct_adb = [
            line.strip()
            for line in HARNESS.splitlines()
            if re.match(r"^\s*adb(?:\s|$)", line)
        ]
        self.assertEqual(direct_adb, ['adb -s "$device" "$@"'])
        guard = HARNESS.split("guard_device_action()", 1)[1].split(
            "adb_device()", 1
        )[0]
        self.assertIn("agentchat.py", HARNESS)
        self.assertIn('--room "$room"', guard)
        self.assertIn("--target 'device:vivo'", guard)
        self.assertIn("--area 'device:vivo'", guard)
        wrapper = HARNESS.split("adb_device()", 1)[1].split(
            "stop_phase_logcat()", 1
        )[0]
        self.assertIn('guard_device_action "adb-$subcommand"', wrapper)

    def test_exact_game_and_graphics_baseline_are_fixed(self):
        self.assertIn("CUSA03173/eboot.bin", HARNESS)
        self.assertIn("serial=CUSA03173", HARNESS)
        self.assertIn("target=960x544", HARNESS)
        for token in (
            "profile_graphics_override true",
            "profile_disable_dynamic_shadows true",
            "profile_disable_ssao true",
            "profile_disable_motion_blur true",
            "profile_disable_depth_of_field true",
            "profile_disable_anti_aliasing true",
            "profile_disable_chromatic_aberration true",
            "assert_graphics_baseline",
            "assert_platform_policy_contract",
            "aggressive40Hz=1 targetNs=25000000",
            "vivo mode=performance-hint-4241",
            "confounding-root-boost-applied",
        ):
            self.assertIn(token, HARNESS)

    def test_default_sequence_is_fresh_process_0_2_0(self):
        ordered = [
            "run_phase baseline-a1 0 '' baseline 0x0 1",
            'run_phase memcpyset-relaxed-b 2 "$relaxed_memcpy_marker" memcpyset-relaxed 0x2 0',
            "run_phase baseline-a2 0 '' baseline 0x0 1",
        ]
        positions = [HARNESS.index(token) for token in ordered]
        self.assertEqual(positions, sorted(positions))
        self.assertIn("stop_game\n    set_phase_marker_state", HARNESS)
        self.assertIn("reason=pid-not-fresh", HARNESS)
        self.assertIn("reason=aba-pids-not-unique", HARNESS)
        self.assertIn('pid="$launched_pid"', HARNESS)
        self.assertIn("sequence=baseline-a1:0,memcpyset-relaxed-b:2,baseline-a2:0", HARNESS)

    def test_only_selected_relaxed_marker_can_be_active(self):
        for token in (
            "run-enable-fex-relaxed-memcpyset-tso-ab",
            "run-enable-fex-relaxed-vector-tso-ab",
            "run-enable-fex-multiblock",
            "run-enable-fex-buffer-capture-ack",
            "run-enable-fex-deferred-write-epoch",
            "run-enable-bounded-guest-invalidation-drain",
            "run-force-fex-software-tso",
            "run-enable-fex-low-memory-cache",
            "run-disable-adpf-40hz-target",
            "run-profile-guest-invalidation-sources",
            "run-profile-guest-invalidation-overlap",
            "assert_unsafe_marker_state",
        ):
            self.assertIn(token, HARNESS)
        marker_setter = HARNESS.split("set_phase_marker_state()", 1)[1].split(
            "assert_graphics_baseline()", 1
        )[0]
        self.assertIn('selected" != "$relaxed_memcpy_marker', marker_setter)
        self.assertNotIn("touch $runtime_home/run-force-fex-software-tso", HARNESS)
        self.assertIn("forceSoftware=0", HARNESS)

    def test_provenance_is_fail_closed(self):
        for token in (
            'status.get("effective") != "fex-2608-experimental"',
            'exact_int("configureFlags")',
            "strong_cap = 1 << 2",
            "ab_override_cap = 1 << 12",
            "capability bit12 absent",
            "baseline capability bit2 absent",
            "relaxed phase still advertises/requires bit2",
            'status.get("strongMemoryModel")',
            "[EXECUTOR_FEX_MEMORY_MODEL_AB]",
            "requestedLowMemory=0 effectiveLowMemory=0 dynamicL1=0 disableL2=0",
            "-lookup-throughput-sm-swtso",
            "-lookup-throughput-ab-mtso0-sm-swtso",
            "cacheSuffixRole=provenance-only",
            "compare_cache_provenance",
        ):
            self.assertIn(token, HARNESS)

    def test_all_loaded_runtime_hashes_match_canonical_artifacts(self):
        for role, library in (
            ("executor", "liblsx4_executor_android.so"),
            ("adapter", "liblsx4_fex_backend_v3.so"),
            ("bridge", "liblsx4_fex2608_context_bridge.so"),
            ("fexcore", "libFEXCore.so"),
        ):
            self.assertIn(role, HARNESS)
            self.assertIn(library, HARNESS)
        self.assertIn("verify_android_fex_artifacts.py", HARNESS)
        self.assertIn("--manifest", HARNESS)
        self.assertIn("loaded-artifact-hash-mismatch", HARNESS)
        self.assertIn("local_artifact_sha", HARNESS)

    def test_measurement_gates_warmup_scene_work_freeze_and_graphics(self):
        for token in (
            "BB_FEX_MEMORY_AB_WARMUP_WAIT",
            "WARMUP_DONE",
            "BB_MIN_WARM_LIVE_SAMPLES",
            "simpleperf stat -p",
            "instructions:u,cpu-cycles:u,task-clock",
            "SurfaceFlinger --timestats -clear",
            "statsStart = ",
            "statsEnd = ",
            "zero-fps-frozen-tail",
            "surfaceflinger-dropped-frames",
            "assert_no_crash_freeze_log",
            "write_builder_scene_metrics",
            "BB_MIN_BUILDER_UNITS_PER_FRAME",
            "compare_aba_scene_metric builderUnitsPerFrame",
            "compare_aba_scene_metric builderDrawPerFrame",
            "compare_aba_scene_metric builderShaderPerFrame",
            "write_png_metrics",
            "blank/corrupt graphics gate",
            "compare_screenshot_graphics",
            "white/missing-geometry gate failed",
            "STATIC_SCENE_POSITIONED",
        ):
            self.assertIn(token, HARNESS)

    def test_cleanup_removes_relaxed_state_and_leaves_exact_baseline_running(self):
        cleanup = HARNESS.split("restore_baseline_running_best_effort()", 1)[1].split(
            "on_exit()", 1
        )[0]
        self.assertIn("remove_unsafe_markers", cleanup)
        self.assertIn("assert_unsafe_marker_state ''", cleanup)
        self.assertIn('if [[ "$active_process_flags" == 0 && -n "$current_pid" ]]', cleanup)
        self.assertIn("CLEANUP_KEEP_RUNNING_BASELINE", cleanup)
        self.assertIn("launch_through_handoff", cleanup)
        self.assertIn("CLEANUP_BASELINE_RUNNING", cleanup)
        self.assertNotRegex(cleanup.rstrip().splitlines()[-1], r"force-stop")
        self.assertIn("device_session_started == 1", HARNESS)
        self.assertIn("finalMode=baseline finalGameRunning=true", HARNESS)


if __name__ == "__main__":
    unittest.main()
