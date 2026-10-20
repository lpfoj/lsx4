import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
JNI = (ROOT / "android-app/app/src/main/cpp/runtime_bridge.cpp").read_text(
    encoding="utf-8"
)
BRIDGE = (
    ROOT / "android-app/app/src/main/java/app/lsx4/android/RuntimeBridge.java"
).read_text(encoding="utf-8")
ACTIVITY = (
    ROOT / "android-app/app/src/main/java/app/lsx4/android/MainActivity.java"
).read_text(encoding="utf-8")


class HostPmuHudContractTests(unittest.TestCase):
    def test_sampler_is_hardware_only_and_inherited(self):
        self.assertIn("PERF_TYPE_HARDWARE", JNI)
        self.assertIn("PERF_COUNT_HW_INSTRUCTIONS", JNI)
        self.assertIn("PERF_COUNT_HW_CPU_CYCLES", JNI)
        self.assertIn("PERF_COUNT_HW_BRANCH_MISSES", JNI)
        self.assertIn("attr.inherit = 1", JNI)
        self.assertIn("attr.exclude_kernel = 1", JNI)
        self.assertNotIn('opendir("/proc/self/task")', JNI)

    def test_permission_failure_stays_fail_closed(self):
        self.assertIn("if (!attempted_)", JNI)
        self.assertIn("if (!available_)", JNI)
        self.assertIn('runtimeHudPmuLabel = "PMU n/a"', ACTIVITY)

    def test_hud_samples_at_most_about_once_per_second(self):
        self.assertIn("public static native long[] hostPmuStats()", BRIDGE)
        self.assertIn("RuntimeBridge.hostPmuStats()", ACTIVITY)
        self.assertIn("now - runtimeHudLastPmuReadAtMs < 900L", ACTIVITY)
        self.assertIn("%.2f Gi/s  IPC %.2f  BM %.2f%%", ACTIVITY)
        self.assertIn("private TextView debugHudPmu", ACTIVITY)
        self.assertIn("debugHudPmu.setTextSize(6.2f)", ACTIVITY)
        self.assertIn('debugHudPmu.setText(metrics == null ? "PMU n/a" : metrics)', ACTIVITY)


if __name__ == "__main__":
    unittest.main()
