import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
NATIVE = (ROOT / "src/executor/native_runtime_api.cpp").read_text(encoding="utf-8")
PTHREAD = (
    ROOT / "funnel-arm/src/core/libraries/kernel/threads/pthread.cpp"
).read_text(encoding="utf-8")


class FexPthreadOnceContractTests(unittest.TestCase):
    def test_active_fex_never_uses_legacy_once_suppression(self):
        gate = NATIVE.split(
            'extern "C" bool executor_lsx4_android_should_suppress_guest_once', 1
        )[1].split(
            'extern "C" void executor_lsx4_android_note_suppressed_guest_once', 1
        )[0]
        self.assertIn("RuntimeGuestJitActive() || RuntimeGuestFexActive()", gate)
        self.assertIn("return false", gate)
        self.assertLess(
            gate.index("RuntimeGuestJitActive() || RuntimeGuestFexActive()"),
            gate.index("g_box64_run_mapped_function == nullptr"),
        )

    def test_fex_once_uses_guest_callback_and_failure_reopens_once(self):
        runtime = NATIVE.split(
            'extern "C" int executor_lsx4_android_run_guest_once', 1
        )[1].split(
            'extern "C" int executor_lsx4_android_run_guest_callback6', 1
        )[0]
        self.assertIn("if (fex_active)", runtime)
        self.assertIn("RunFexGuestCallbackN", runtime)
        self.assertIn("status == LSX4_FEX_STATUS_OK ? 1 : status", runtime)

        funnel = PTHREAD.split("int PS4_SYSV_ABI posix_pthread_once", 1)[1].split(
            "int PS4_SYSV_ABI posix_pthread_key_create", 1
        )[0]
        self.assertIn("guest_once_result > 0", funnel)
        self.assertIn("PthreadOnceState::Done", funnel)
        self.assertIn("guest_once_result < 0", funnel)
        self.assertIn("PthreadOnceState::NeverDone", funnel)
        self.assertIn("notify_all", funnel)


if __name__ == "__main__":
    unittest.main()
