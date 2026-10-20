import importlib.util
import pathlib
import sys
import unittest
from unittest import mock


ROOT = pathlib.Path(__file__).resolve().parents[1]
MODULE_PATH = ROOT / "tools" / "bloodborne-proof" / "capture_live_environment.py"
SPEC = importlib.util.spec_from_file_location("bloodborne_live_environment", MODULE_PATH)
live = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
sys.modules[SPEC.name] = live
SPEC.loader.exec_module(live)


class LiveEnvironmentParserTests(unittest.TestCase):
    @staticmethod
    def environment_fixture():
        return {
            "thermal": {"text": "\n".join([
                "Thermal Status: 2",
                "Current temperatures from HAL:",
                "Temperature{mValue=42.0, mType=3, mName=skin, mStatus=2}",
                "Temperature{mValue=50.0, mType=0, mName=CPU4, mStatus=0}",
                "Temperature{mValue=48.0, mType=1, mName=GPU7, mStatus=0}",
                "Current cooling devices from HAL:",
            ])},
            "battery": {"text": "\n".join([
                "  AC powered: false",
                "  USB powered: true",
                "  Wireless powered: false",
                "  Dock powered: false",
                "  status: 2",
                "  level: 61",
                "  voltage: 4175",
            ])},
            "power": {"text": "\n".join([
                "  mWakefulness=Awake",
                "  Battery Saver is currently: OFF",
            ])},
            "cpuFreq": {"text": "\n".join([
                "/sys/devices/system/cpu/cpufreq/policy0 affected_cpus=0 1 2 3 4 5",
                "scaling_cur_freq=1555200",
                "scaling_min_freq=1555200",
                "scaling_max_freq=1555200",
                "/sys/devices/system/cpu/cpufreq/policy6 affected_cpus=6",
                "scaling_cur_freq=2304000",
                "scaling_min_freq=2304000",
                "scaling_max_freq=2304000",
            ])},
            "gpuFreq": {"text": (
                "source=kgsl-sysfs status=unavailable "
                "reason=no-readable-frequency-node\n"
            )},
        }

    def test_current_hal_section_is_used_and_bcl_is_not_called_celsius(self):
        text = "\n".join([
            "cached:",
            "Temperature{mValue=99.0, mType=3, mName=skin, mStatus=3}",
            "Current temperatures from HAL:",
            "Temperature{mValue=3.098, mType=6, mName=vbat, mStatus=6}",
            "Temperature{mValue=47.281, mType=3, mName=skin, mStatus=3}",
            "Temperature{mValue=63.6, mType=0, mName=CPU4, mStatus=0}",
            "Temperature{mValue=59.7, mType=1, mName=GPU7, mStatus=0}",
            "Current cooling devices from HAL:",
        ])
        result = live.parse_temperatures(text)
        self.assertEqual(result["skin"]["value"], 47.281)
        self.assertEqual(result["skin"]["unit"], "celsius")
        vbat = next(item for item in result["allCurrentHal"] if item["name"] == "vbat")
        self.assertEqual(vbat["unit"], "hal-native")
        self.assertNotIn("celsius", vbat)

    def test_required_surface_cpu_and_gpu_sensors_are_fail_closed(self):
        text = "\n".join([
            "Current temperatures from HAL:",
            "Temperature{mValue=47.0, mType=3, mName=skin, mStatus=3}",
            "Current cooling devices from HAL:",
        ])
        with self.assertRaisesRegex(
            live.proof.CaptureRejected, "required-current-temperature-missing"
        ):
            live.parse_temperatures(text)

    def test_cpu_frequency_requires_complete_policy_fields(self):
        good = "\n".join([
            "/sys/cpufreq/policy0 affected_cpus=0 1 2 3 4 5",
            "scaling_cur_freq=1555200",
            "scaling_min_freq=1555200",
            "scaling_max_freq=1555200",
        ])
        self.assertEqual(live.parse_cpu_frequencies(good)[0]["currentKHz"], 1555200)
        with self.assertRaisesRegex(
            live.proof.CaptureRejected, "cpu-frequency-parse-failed"
        ):
            live.parse_cpu_frequencies(good.rsplit("\n", 1)[0])

    def test_environment_summary_preserves_admission_inputs(self):
        result = live.summarize_environment(self.environment_fixture())
        self.assertEqual(result["thermal"]["globalStatus"], 2)
        self.assertEqual(result["battery"]["levelPercent"], 61)
        self.assertEqual(result["battery"]["status"], 2)
        self.assertTrue(result["battery"]["powered"]["usb"])
        self.assertTrue(result["battery"]["externalPower"])
        self.assertEqual(result["power"]["batterySaver"], "OFF")
        self.assertEqual(
            [item["policy"] for item in result["cpuFrequencies"]], [0, 6]
        )
        self.assertEqual(result["gpuFrequency"]["status"], "unavailable")
        self.assertFalse(result["gpuFrequency"]["proxyUsed"])

    def test_readiness_allows_absent_game_without_launching_it(self):
        responses = {
            "serial": "10AG263F1B001HM\n",
            "boot-id": "boot-1\n",
            "device-uptime": "321.5 100.0\n",
            "game-pid": "",
            "activity": (
                "  topResumedActivity=ActivityRecord{abc u0 "
                "com.android.launcher/.Launcher t1}\n"
            ),
        }

        class FakeAdb:
            def __init__(self):
                self.labels = []

            def text(self, label, _command, **_kwargs):
                self.labels.append(label)
                return responses[label]

        adb = FakeAdb()
        with mock.patch.object(
            live.proof, "capture_environment", return_value=self.environment_fixture()
        ):
            result = live.collect_readiness(adb)
        self.assertFalse(result["foreground"]["gameProcessPresent"])
        self.assertIsNone(result["foreground"]["gamePid"])
        self.assertEqual(result["device"]["serial"], "10AG263F1B001HM")
        self.assertNotIn("launch", adb.labels)


if __name__ == "__main__":
    unittest.main()
