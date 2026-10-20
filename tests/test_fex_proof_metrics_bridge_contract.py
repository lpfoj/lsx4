import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
LOADER_H = (ROOT / "src/executor/fex_backend_loader.h").read_text(encoding="utf-8")
LOADER_CPP = (ROOT / "src/executor/fex_backend_loader.cpp").read_text(encoding="utf-8")
RUNTIME = (ROOT / "src/executor/native_runtime_api.cpp").read_text(encoding="utf-8")
JNI = (ROOT / "android-app/app/src/main/cpp/runtime_bridge.cpp").read_text(
    encoding="utf-8"
)
JAVA = (ROOT / "android-app/app/src/main/java/app/lsx4/android/RuntimeBridge.java").read_text(
    encoding="utf-8"
)
RECEIVER = (
    ROOT / "android-app/app/src/main/java/app/lsx4/android/ProofMetricsReceiver.java"
).read_text(encoding="utf-8")
MANIFEST = (ROOT / "android-app/app/src/main/AndroidManifest.xml").read_text(
    encoding="utf-8"
)


class FexProofMetricsBridgeContractTests(unittest.TestCase):
    def test_query_is_a_separate_optional_abi(self):
        self.assertIn("fex_proof_metrics_abi_v1.h", LOADER_H)
        self.assertIn("QueryProofMetrics(Lsx4FexProofMetricsV1& metrics)", LOADER_H)
        self.assertIn("LSX4_FEX_BACKEND_PROOF_METRICS_QUERY_V1_SYMBOL", LOADER_CPP)
        self.assertIn("LSX4_FEX_STATUS_NOT_FOUND", LOADER_CPP)

    def test_default_off_and_conflicting_markers_fail_closed(self):
        self.assertIn('root / "diag-fex-proof-metrics-static"', RUNTIME)
        self.assertIn('root / "diag-fex-proof-metrics-dynamic"', RUNTIME)
        self.assertIn("proof_metrics_static && proof_metrics_dynamic", RUNTIME)
        self.assertIn('::unsetenv("LSX4_FEX2608_DIAG_PROOF_METRICS")', RUNTIME)
        self.assertIn('"fex-proof-metrics-markers-conflict"', RUNTIME)

    def test_runtime_json_keeps_validity_and_semantic_masks(self):
        for field in (
            '"requestedMask"',
            '"validMask"',
            '"unavailableMask"',
            '"exactMask"',
            '"sampledMask"',
            '"staticMask"',
            '"dynamicMask"',
            '"perturbedMask"',
            '"attemptedNotRetiredMask"',
            '"bootBeforeNs"',
            '"bootAfterNs"',
        ):
            self.assertIn(field, RUNTIME)
        self.assertIn('"executedDispatcherEntries"', RUNTIME)
        self.assertIn('"compiledDmbSites"', RUNTIME)

    def test_external_snapshot_uses_unique_token_and_atomic_file(self):
        self.assertIn("g_fex_proof_metrics", JNI)
        self.assertIn("RuntimeBridge_fexProofMetrics", JNI)
        self.assertIn("runtimeProofCounters", JAVA)
        self.assertIn("app.lsx4.android.CAPTURE_PROOF_METRICS", RECEIVER)
        self.assertIn('[A-Za-z0-9._-]{1,80}', RECEIVER)
        self.assertIn("request token already exists", RECEIVER)
        self.assertIn("StandardCopyOption.ATOMIC_MOVE", RECEIVER)
        self.assertNotIn("StandardCopyOption.REPLACE_EXISTING", RECEIVER)
        self.assertIn('payload.put("processStarttime"', RECEIVER)
        self.assertIn('payload.put("fex"', RECEIVER)
        self.assertIn('payload.put("runtime"', RECEIVER)
        self.assertIn('payload.put("gpuResidency"', RECEIVER)
        self.assertIn('payload.put("androidMemory"', RECEIVER)
        self.assertIn("RuntimeBridge.gpuResidencyMetrics()", RECEIVER)
        self.assertIn('"lsx4-proof-metrics-snapshot/v2"', RECEIVER)
        self.assertIn("RuntimeBridge.runtimeHudStats()", RECEIVER)
        self.assertIn('"lsx4-runtime-hud-counters/v1"', RECEIVER)
        self.assertIn('hudCounters.put("drawCalls"', RECEIVER)
        self.assertIn('hudCounters.put("submitCalls"', RECEIVER)
        self.assertIn('hudCounters.put("presentCalls"', RECEIVER)
        self.assertIn('payload.put("hud"', RECEIVER)

    def test_snapshot_collects_os_graphics_memory_only_on_explicit_proof_request(self):
        self.assertIn("Debug.getMemoryInfo(memory)", RECEIVER)
        self.assertIn('"lsx4-android-process-memory/v1"', RECEIVER)
        self.assertIn('"android.os.Debug.MemoryInfo-explicit-proof-only"', RECEIVER)
        self.assertIn('memoryStatKb(memory, "summary.graphics")', RECEIVER)
        self.assertIn('result.put("summaryGraphicsKb"', RECEIVER)
        self.assertIn('result.put("totalPssKb"', RECEIVER)
        self.assertNotIn("ActivityManager.getProcessMemoryInfo", RECEIVER)

    def test_snapshot_process_identity_stays_on_java8_android_api(self):



        self.assertNotIn("Files.readString(", RECEIVER)
        self.assertIn("Files.readAllBytes", RECEIVER)
        self.assertIn('new File("/proc/self/stat")', RECEIVER)

    def test_snapshot_receiver_is_shell_only_and_stays_in_game_process(self):
        self.assertIn('android:name="app.lsx4.android.ProofMetricsReceiver"', MANIFEST)
        self.assertIn('android:permission="android.permission.DUMP"', MANIFEST)
        self.assertIn('android:process=":game"', MANIFEST)
        self.assertIn('android:exported="true"', MANIFEST)
        self.assertNotIn("SMOKE_FEX_PROOF_METRICS", (
            ROOT / "android-app/app/src/main/java/app/lsx4/android/MainActivity.java"
        ).read_text(encoding="utf-8"))


if __name__ == "__main__":
    unittest.main()
