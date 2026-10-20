from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
MEASURE = (ROOT / "scripts/measure-bloodborne-gameplay-vivo.ps1").read_text(
    encoding="utf-8"
)
POCO = (ROOT / "scripts/measure-bloodborne-gameplay-poco-f7-pro.ps1").read_text(
    encoding="utf-8"
)


class PocoF7ProMeasurementRouteContractTests(unittest.TestCase):
    def test_shared_measurement_has_explicit_device_profile(self):
        self.assertIn("[ValidateSet('Vivo', 'PocoF7Pro')]", MEASURE)
        self.assertIn("$performancePolicy", MEASURE)
        self.assertIn("__PERF_POLICY__", MEASURE)
        self.assertIn("$thermalSensor", MEASURE)
        self.assertIn("quiet_therm", MEASURE)

    def test_poco_wrapper_selects_its_own_route_and_cleanup(self):
        self.assertIn("-DeviceProfile PocoF7Pro", POCO)
        self.assertIn("measure-bloodborne-gameplay-vivo.ps1", POCO)
        self.assertIn("preflight-poco-f7-pro-performance-test.ps1", MEASURE)
        self.assertNotIn("start-bloodborne-gameplay-vivo.ps1", POCO)

    def test_poco_measurement_disables_observer_screencap_by_default(self):
        self.assertIn("[bool]$CaptureUnexpectedScreen = $false", POCO)
        self.assertIn(
            "-CaptureUnexpectedScreen $CaptureUnexpectedScreen", POCO
        )
        self.assertIn("$CaptureUnexpectedScreen", MEASURE)
        self.assertIn("screenshot capture disabled", MEASURE)

    def test_vivo_default_and_legacy_summary_fields_remain(self):
        self.assertIn("[string]$DeviceProfile = 'Vivo'", MEASURE)
        self.assertIn("$summary.policy6MedianGHz", MEASURE)
        self.assertIn("$summary.tzShellStartC", MEASURE)
        self.assertIn("$summary.tzShellEndC", MEASURE)


if __name__ == "__main__":
    unittest.main()
