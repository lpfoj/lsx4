from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
HINT_CPP = (ROOT / "src/executor/android_performance_hint.cpp").read_text(
    encoding="utf-8"
)
HINT_H = (ROOT / "src/executor/android_performance_hint.h").read_text(
    encoding="utf-8"
)
NATIVE = (ROOT / "src/executor/native_runtime_api.cpp").read_text(
    encoding="utf-8"
)


class AdpfTargetPolicyContract(unittest.TestCase):
    def test_full_rate_and_relaxed_targets_remain_available(self):
        self.assertIn("FullRateProducerCycleDurationNs = 16'666'667", HINT_CPP)
        self.assertIn("RelaxedProducerCycleDurationNs = 33'333'333", HINT_CPP)
        self.assertIn("SetFullRate60HzTarget(bool enabled) noexcept", HINT_H)

    def test_live_session_is_updated_without_recreation_on_success(self):
        body = HINT_CPP.split("void SetFullRate60HzTarget", 1)[1].split(
            "void SetEnabled", 1
        )[0]
        self.assertIn("state.update_target(state.session, target)", body)
        self.assertIn("if (update_result != 0)", body)
        self.assertIn("CloseSession(state)", body)
        self.assertIn("[EXECUTOR_ADPF_TARGET]", body)

    def test_60hz_policy_is_default_and_relaxed_marker_is_applied_before_preflight(self):
        marker = NATIVE.index('root / "run-disable-adpf-60hz-target"')
        default_policy = NATIVE.index(
            "const bool full_rate_adpf_target = !relaxed_adpf_target", marker
        )
        configure = NATIVE.index("SetFullRate60HzTarget", marker)
        preflight = NATIVE.index("Executor::Fex::Preflight(preflight)", marker)
        self.assertLess(marker, default_policy)
        self.assertLess(default_policy, configure)
        self.assertLess(marker, configure)
        self.assertLess(configure, preflight)


if __name__ == "__main__":
    unittest.main()
