import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
HARNESS = (ROOT / "tools" / "codex_bloodborne_cpu_floor_ab.sh").read_text(
    encoding="utf-8"
)


class VivoPolicyAbHarnessContractTests(unittest.TestCase):
    def test_runs_mutually_exclusive_aba_and_restores_default(self):
        measured = HARNESS.split(
            "measure_phase hint-a1 performance-hint-4241", 1
        )[1]
        ordered = [
            "touch $policy_marker",
            "measure_phase moderate-b moderate-cpu-floor",
            "rm -f '$policy_marker'",
            "measure_phase hint-a2 performance-hint-4241",
        ]
        positions = [measured.index(token) for token in ordered]
        self.assertEqual(positions, sorted(positions))
        self.assertIn("finalPolicy=performance-hint-4241 gameRunning=true", HARNESS)
        self.assertIn("run-use-vivo-moderate-cpu-floor", HARNESS)
        self.assertNotIn("ensure_disable_marker_absent", HARNESS)
        self.assertNotIn("$disable_marker", HARNESS)

    def test_release_before_acquire_is_correlated_by_handle(self):
        transition = HARNESS.split("transition_order()", 1)[1].split(
            "if ! transition_order", 1
        )[0]
        for token in (
            "oldHandle=",
            "newHandle=",
            "result=released handle=$old_handle",
            "result=acquired handle=$new_handle",
            "detach < release",
            "release < released",
            "released < acquire",
            "acquire < commit",
            "commit < acquired",
        ):
            self.assertIn(token, transition)

    def test_rejects_policy_and_scene_confounders(self):
        for token in (
            "ensure_root_boost_disabled",
            "managed_oem_root_performance",
            "CONFOUNDING_ROOT_BOOST_APPLIED",
            "assert_policy_marker_state",
            "PHASE_INVALID_POLICY_SWITCH",
            "PHASE_INVALID_UNEXPECTED_POLICY",
            "gameState loading=false gameplay=true",
            "serial=CUSA03173",
            "target=960x544",
            "write_builder_scene_metrics",
            "BB_MIN_BUILDER_UNITS_PER_FRAME",
            "ABA_SCENE_MISMATCH",
            "compare_aba_scene_metric builderUnitsPerFrame",
            "compare_aba_scene_metric builderDrawPerFrame",
            "compare_aba_scene_metric builderShaderPerFrame",
            "scene_metric moderate-b",
            "BB_MIN_BUILDER_UNITS_PER_FRAME:-400",
        ):
            self.assertIn(token, HARNESS)

    def test_cumulative_policy_failures_cover_phase_transition_gaps(self):
        self.assertIn("assert_no_policy_failures()", HARNESS)
        for token in (
            "release-retry-scheduled",
            "release-retry-exhausted",
            "release-pending-expired",
            "acquire-blocked",
            "before-hint-marker",
            "before-moderate-marker",
            "aba-complete",
        ):
            if token == "before-hint-marker":
                token = "before-hint-default"
            self.assertIn(token, HARNESS)

    def test_measurement_validates_real_windows_and_liveness(self):
        for token in (
            "statsStart = ",
            "statsEnd = ",
            "PHASE_INVALID_SF_WINDOW",
            "PHASE_INVALID_PMU_COVERAGE",
            "PHASE_INVALID_FROZEN_TAIL",
            "PHASE_INVALID_PID",
            "PHASE_INVALID_NOT_FOREGROUND",
        ):
            self.assertIn(token, HARNESS)

    def test_every_adb_transport_call_goes_through_guard(self):
        direct = [
            line.strip()
            for line in HARNESS.splitlines()
            if line.lstrip().startswith("adb ")
        ]
        self.assertEqual(direct, ['adb -s "$device" "$@"'])
        self.assertIn('guard_device_action "adb-$1"', HARNESS)
        self.assertIn('--target "device:vivo"', HARNESS)
        self.assertIn('--area "device:vivo"', HARNESS)


if __name__ == "__main__":
    unittest.main()
