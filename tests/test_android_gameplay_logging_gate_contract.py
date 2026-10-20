from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "src/executor/native_runtime_api.cpp").read_text(encoding="utf-8")


class AndroidGameplayLoggingGateContractTests(unittest.TestCase):
    def _game_config_body(self) -> str:
        start = SOURCE.index("void LoadAndroidGameSpecificConfig(std::string_view title_id) {")
        end = SOURCE.index("#ifdef __ANDROID__\nextern \"C\" void executor_lsx4_reinstall_bus_handler", start)
        return SOURCE[start:end]

    def test_quiet_override_is_applied_after_title_config_load(self):
        body = self._game_config_body()
        load = body.index("Config::load(config, true);")
        quiet = body.index('std::getenv("EXECUTOR_LIVE_QUIET")')
        disable = body.index("Config::setLoggingEnabled(false, true);")

        self.assertLess(load, quiet)
        self.assertLess(quiet, disable)

    def test_gameplay_logging_requires_exact_positive_opt_in(self):
        body = self._game_config_body()
        marker = body.index('"run-enable-gameplay-logging"')
        marker_check = body.index("std::filesystem::is_regular_file", marker)
        opt_in_gate = body.index("if (!gameplay_logging_opt_in)", marker_check)
        disable = body.index("Config::setLoggingEnabled(false, true);", opt_in_gate)

        self.assertLess(marker, marker_check)
        self.assertLess(marker_check, opt_in_gate)
        self.assertLess(opt_in_gate, disable)
        self.assertNotIn("Config::setLoggingEnabled(true", body)

    def test_confirmation_uses_native_log_and_reports_effective_state(self):
        body = self._game_config_body()
        self.assertIn("[EXECUTOR_GAMEPLAY_LOGGING]", body)
        self.assertIn("Config::getLoggingEnabled() ? 1 : 0", body)
        self.assertIn('"explicit-opt-in" : "production-quiet"', body)
        self.assertIn("NativeLog(ANDROID_LOG_INFO", body)


if __name__ == "__main__":
    unittest.main()
