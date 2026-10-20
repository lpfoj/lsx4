import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PTHREAD_SPEC = (
    ROOT / "funnel-arm/src/core/libraries/kernel/threads/pthread_spec.cpp"
).read_text(encoding="utf-8")
THREAD_STATE = (
    ROOT / "funnel-arm/src/core/libraries/kernel/threads/thread_state.cpp"
).read_text(encoding="utf-8")
NATIVE = (ROOT / "src/executor/native_runtime_api.cpp").read_text(encoding="utf-8")
MUTEX = (
    ROOT / "funnel-arm/src/core/libraries/kernel/threads/mutex.cpp"
).read_text(encoding="utf-8")


class AndroidHleTlsHotpathContractTests(unittest.TestCase):
    def test_disabled_tls_tracing_uses_process_immutable_cached_gates(self):
        light = PTHREAD_SPEC.split("static bool ExecutorLightOracleMode", 1)[1].split(
            "static bool ConsumeLightOracleTlsBudget", 1
        )[0]
        for required in (
            'static const bool enabled = std::getenv("EXECUTOR_LIGHT_ORACLE")',
            'static const bool trace_live_wide =',
            'std::getenv("EXECUTOR_TRACE_LIVE_WIDE")',
            'static const bool trace_live_tls =',
            'std::getenv("EXECUTOR_TRACE_LIVE_TLS")',
            'static const bool trace_render_thread_only =',
        ):
            self.assertIn(required, light)

    def test_current_pthread_reuses_one_hot_g_curthread_result(self):
        current = THREAD_STATE.split("Pthread* CurrentOrFallbackPthread()", 1)[1].split(
            "char host_name[64]", 1
        )[0]
        self.assertIn("Pthread* const current_thread = g_curthread;", current)
        self.assertIn("g_host_bound_thread = current_thread;", current)
        self.assertIn("return current_thread;", current)
        self.assertEqual(current.count("= g_curthread;"), 1)
        self.assertNotIn("return g_curthread;", current)

    def test_fex_hle_scope_resolves_depth_tls_once_and_reuses_guest_pointer(self):
        scope = NATIVE.split("class ScopedFexHleDispatchDepth", 1)[1].split(
            "class ScopedFexBulkLeaseAbort", 1
        )[0]
        self.assertIn("depth(&g_fex_hle_dispatch_depth)", scope)
        self.assertIn("TryEnterDepth(*depth)", scope)
        self.assertIn("LeaveDepth(*depth)", scope)
        self.assertEqual(scope.count("g_fex_hle_dispatch_depth"), 1)
        self.assertEqual(
            NATIVE.count("ScopedFexHleDispatchDepth dispatch_depth(current_guest_thread)"),
            3,
        )

    def test_atomic_delivery_cleanup_preserves_a_new_internal_requeue(self):
        clear = MUTEX.split("static void ClearMonoSuspendAbbaSignalState", 1)[1].split(
            "static bool TryMarkUnityPreloadControlSignalActive", 1
        )[0]
        self.assertIn("preserve_atomic_requeue", clear)
        self.assertIn("FexHleSigusr1DeliveryBit", clear)
        self.assertLess(
            clear.index("if (preserve_atomic_requeue)"),
            clear.index("g_mono_suspend_pending_sigusr1.erase(target)"),
        )


if __name__ == "__main__":
    unittest.main()
