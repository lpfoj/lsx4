import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
ACTIVITY = (ROOT / "android-app/app/src/main/java/app/lsx4/android/MainActivity.java").read_text(
    encoding="utf-8"
)


class GuestCpuBackendStatusPersistenceContractTests(unittest.TestCase):
    def test_selection_and_successful_initialize_use_atomic_status_publish(self):
        helper = ACTIVITY.split(
            "private void persistGuestCpuBackendStatus", 1
        )[1].split("private String runtimeLibraryName", 1)[0]
        self.assertIn('new File(getFilesDir(), "guest-cpu-backend-status.json")', helper)
        self.assertIn("writeBytesAtomically(", helper)

        selection = ACTIVITY.split("private void applyGuestCpuBackendSelection", 1)[1].split(
            "private void persistGuestCpuBackendStatus", 1
        )[0]
        self.assertIn('persistGuestCpuBackendStatus("selected")', selection)

        initialize = ACTIVITY.split("private void initializeRuntime()", 1)[1].split(
            "private void refreshRuntimeSnapshot", 1
        )[0]
        status_refresh = initialize.index(
            "guestCpuBackendStatus = RuntimeBridge.guestCpuBackendStatus();"
        )
        success_gate = initialize.index("if (result == 0)", status_refresh)
        persist = initialize.index(
            'persistGuestCpuBackendStatus("initialized")', success_gate
        )
        log = initialize.index(
            'Log.i(TAG, "EXECUTOR_GUEST_CPU_BACKEND phase=initialized status="', persist
        )
        self.assertLess(status_refresh, success_gate)
        self.assertLess(success_gate, persist)
        self.assertLess(persist, log)


if __name__ == "__main__":
    unittest.main()
