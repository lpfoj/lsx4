import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
ROUTE = (ROOT / "scripts" / "start-bloodborne-gameplay-vivo.ps1").read_text(
    encoding="utf-8"
)
MEASURE = (ROOT / "scripts" / "measure-bloodborne-gameplay-vivo.ps1").read_text(
    encoding="utf-8"
)
CLASSIFIER = (ROOT / "tools" / "bloodborne-proof" / "classify_screen.py").read_text(
    encoding="utf-8"
)


class BloodborneFastRouteContract(unittest.TestCase):
    def test_route_is_state_driven_and_has_exact_gameplay_gate(self):
        self.assertIn("classify_screen.py", ROUTE)
        self.assertIn("Get-RuntimeDrawRate", ROUTE)
        self.assertIn("GameplayDrawCallsPerSecond", ROUTE)
        self.assertNotIn("WarningWaitSeconds", ROUTE)
        self.assertNotIn("OfflineWaitSeconds", ROUTE)

    def test_cross_after_continue_requires_positive_menu_reclassification(self):
        self.assertIn("if ($continueSent)", ROUTE)
        self.assertIn("[string]$screen.state -eq 'continue-menu'", ROUTE)
        self.assertIn("never infer it from low draw alone", ROUTE)

    def test_bright_gameplay_recovery_is_draw_gated_and_input_free(self):
        start = ROUTE.index("if ($state -eq 'startup-clickable')")
        end = ROUTE.index("$canPress =", start)
        recovery = ROUTE[start:end]
        self.assertIn("Get-RuntimeDrawRate", recovery)
        self.assertIn("Get-GameAverageUsedCores", recovery)
        self.assertIn("recovery=direct-or-post-offline-draw-gate", recovery)
        self.assertNotIn("Press-Cross", recovery)

    def test_expected_frames_are_temporary_and_failure_frame_is_persisted(self):
        self.assertIn("GetTempPath", ROUTE)
        self.assertIn("Save-UnexpectedScreen", ROUTE)
        self.assertIn("Remove-Item -LiteralPath $routeScreen", ROUTE)
        self.assertIn("Copy-Item -LiteralPath $routeScreen", ROUTE)

    def test_classifier_uses_structure_without_game_templates(self):
        self.assertIn('state = "online-menu"', CLASSIFIER)
        self.assertIn('state = "continue-menu"', CLASSIFIER)
        self.assertIn('state = "dark-transition"', CLASSIFIER)
        self.assertNotIn("matchTemplate", CLASSIFIER)

    def test_measurement_records_runtime_rates_and_closes_after_phase(self):
        self.assertIn("runtimeDrawCallsPerSecond", MEASURE)
        self.assertIn("runtimeSubmitCallsPerSecond", MEASURE)
        self.assertIn("runtimePresentCallsPerSecond", MEASURE)
        self.assertIn("fexJitCompilationsDelta", MEASURE)
        self.assertIn("am', 'force-stop', $package", MEASURE)


if __name__ == "__main__":
    unittest.main()
