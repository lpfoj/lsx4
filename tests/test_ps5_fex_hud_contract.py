import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
RUNTIME = (ROOT / "src/executor/ps5_desktop/runtime_api.cpp").read_text(
    encoding="utf-8"
)
PS4_RUNTIME = (ROOT / "src/executor/native_runtime_api.cpp").read_text(
    encoding="utf-8"
)
JNI_BRIDGE = (ROOT / "android-app/app/src/main/cpp/runtime_bridge.cpp").read_text(
    encoding="utf-8"
)
PROOF_RECEIVER = (
    ROOT
    / "android-app/app/src/main/java/app/lsx4/android/ProofMetricsReceiver.java"
).read_text(encoding="utf-8")
ACTIVITY = (
    ROOT
    / "android-app/app/src/main/java/app/lsx4/android/MainActivity.java"
).read_text(encoding="utf-8")


class Ps5FexHudContractTests(unittest.TestCase):
    def test_ps5_hud_exports_the_fex_discriminator_and_real_block_count(self):
        hud = RUNTIME.split(
            'extern "C" int executor_lsx4_ps5_runtime_hud_stats(', 1
        )[1].split(
            'extern "C" int executor_lsx4_ps5_runtime_set_managed_optimization(',
            1,
        )[0]
        self.assertIn("ValueCount = 99u", hud)
        self.assertIn("Executor::Fex::ActiveApi()", hud)
        self.assertIn("fex_api->query", hud)
        self.assertIn("result.published_executable_blocks", hud)
        self.assertIn("values[92] = fex_api != nullptr ? 1u : 0u", hud)
        self.assertIn("values[97] = fex_jit_compilations", hud)
        self.assertIn("values[98] = fex_jit_compile_time_ns", hud)

    def test_android_overlay_uses_fex_telemetry_without_exposing_backend_identity(self):
        self.assertIn("current.length < 99 || current[92] == 0", ACTIVITY)
        self.assertIn('"JIT %,d/s  %.2fc  CC %.1fMiB"', ACTIVITY)
        self.assertNotIn('"FEX blocks %,d"', ACTIVITY)
        self.assertNotIn('"FEX JIT %,d/s', ACTIVITY)

    def test_ps4_and_android_append_the_same_jit_telemetry_tail(self):
        hud = PS4_RUNTIME.split(
            'extern "C" int executor_lsx4_runtime_hud_stats(', 1
        )[1].split(
            'extern "C" const char* executor_lsx4_runtime_jit_selftest()', 1
        )[0]
        self.assertIn("kValueCount = 166", hud)
        self.assertIn("values[97] = fex_jit_compilations", hud)
        self.assertIn("values[98] = fex_jit_compile_time_ns", hud)
        self.assertIn("kValueCapacity = 166", JNI_BRIDGE)
        self.assertIn('.put("fexJitCompilations", 97)', PROOF_RECEIVER)
        self.assertIn('.put("fexJitCompileTimeNs", 98)', PROOF_RECEIVER)


if __name__ == "__main__":
    unittest.main()
