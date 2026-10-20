from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (
    ROOT / "funnel-arm/src/core/libraries/kernel/threads/event_flag.cpp"
).read_text(encoding="utf-8")


def function_body(signature: str, next_signature: str) -> str:
    start = SOURCE.index(signature)
    end = SOURCE.index(next_signature, start)
    return SOURCE[start:end]


class EventFlagDiagnosticsGateContractTests(unittest.TestCase):
    def test_diagnostics_are_cached_positive_opt_in(self):
        start = SOURCE.index("static bool EventFlagDiagnosticsEnabled() {")
        end = SOURCE.index("static bool IsWatchedEventFlag", start)
        gate = SOURCE[start:end]

        self.assertIn("static const bool enabled", gate)
        self.assertIn("config.light_oracle", gate)
        self.assertIn("config.live_wide", gate)
        self.assertIn("config.live_sync", gate)
        self.assertIn("run-diagnostic-event-flag", gate)
        self.assertNotIn("run-live-quiet", gate)

    def test_trace_scans_and_wait_watch_are_behind_gate(self):
        watched = function_body(
            "static bool IsWatchedEventFlag(OrbisKernelEventFlag ef, bool mark) {",
            "static void TraceLiveEventFlag(",
        )
        self.assertLess(
            watched.index("if (!EventFlagDiagnosticsEnabled())"),
            watched.index("static std::atomic<std::uintptr_t> watched"),
        )

        trace_start = SOURCE.index(
            "static void TraceLiveEventFlag(const char* op, OrbisKernelEventFlag ef, u64 before",
            SOURCE.index("static bool IsWatchedEventFlag(OrbisKernelEventFlag ef, bool mark) {"),
        )
        trace_end = SOURCE.index("static void TraceLiveEventFlagDuration(", trace_start)
        trace = SOURCE[trace_start:trace_end]
        self.assertLess(
            trace.index("if (!EventFlagDiagnosticsEnabled())"),
            trace.index("IsCurrentLiveInterestingThread()"),
        )

        watch = function_body(
            "static std::shared_ptr<std::atomic_bool> BeginLiveEventFlagWaitWatch(",
            "#endif\n\nint PS4_SYSV_ABI sceKernelCreateEventFlag",
        )
        self.assertLess(
            watch.index("if (!EventFlagDiagnosticsEnabled())"),
            watch.index("std::make_shared<std::atomic_bool>"),
        )

    def test_functional_event_operations_remain_unconditional(self):
        operations = (
            ("sceKernelClearEventFlag(", "sceKernelCancelEventFlag(", "ef->Clear(bitPattern);"),
            ("sceKernelCancelEventFlag(", "sceKernelSetEventFlag(", "ef->Cancel(setPattern, pNumWaitThreads);"),
            ("sceKernelSetEventFlag(", "sceKernelPollEventFlag(", "ef->Set(bitPattern);"),
            ("sceKernelPollEventFlag(", "sceKernelWaitEventFlag(", "ef->Poll(bitPattern, wait, clear, pResultPat);"),
            ("sceKernelWaitEventFlag(", "void RegisterKernelEventFlag(", "ef->Wait(bitPattern, wait, clear, pResultPat, pTimeout);"),
        )
        for signature, next_signature, operation in operations:
            body = function_body(signature, next_signature)
            op = body.index(operation)
            pre_gate_end = body.rfind("    }\n", 0, op)
            post_gate = body.find("if (diagnostics_enabled)", op)
            self.assertNotEqual(pre_gate_end, -1, signature)
            self.assertNotEqual(post_gate, -1, signature)
            self.assertLess(pre_gate_end, op, signature)
            self.assertLess(op, post_gate, signature)

    def test_wait_timing_and_debug_reads_are_diagnostic_only(self):
        wait = function_body("sceKernelWaitEventFlag(", "void RegisterKernelEventFlag(")
        normal_path = wait.index("const bool diagnostics_enabled = EventFlagDiagnosticsEnabled();")
        gate = wait.index("if (diagnostics_enabled)", normal_path)
        operation = wait.index("ef->Wait(bitPattern, wait, clear, pResultPat, pTimeout);")
        first_clock = wait.index("std::chrono::steady_clock::now()")
        second_gate = wait.index("if (diagnostics_enabled)", operation)
        second_clock = wait.index("std::chrono::steady_clock::now()", second_gate)

        self.assertLess(gate, first_clock)
        self.assertLess(first_clock, operation)
        self.assertLess(operation, second_gate)
        self.assertLess(second_gate, second_clock)
        self.assertIn("before = ef->DebugBits();", wait[gate:operation])
        self.assertNotIn("ef->DebugBits()", wait[normal_path:gate])

    def test_mono_resume_functional_contract_is_preserved(self):
        self.assertIn("RunMonoPendingSignalSafePointFromEventFlagWait(lock);", SOURCE)
        self.assertIn("g_executor_live_mono_resume_event.store(*ef, std::memory_order_release);", SOURCE)
        self.assertIn("g_executor_live_mono_resume_event.compare_exchange_strong", SOURCE)
        self.assertIn("g_executor_live_mono_resume_waiters_to_release.compare_exchange_strong", SOURCE)
        self.assertIn("ExecutorAllowAndroidMonoResumeEventRelease()", SOURCE)
        self.assertIn("ExecutorAllowAndroidMonoPostAckResumeEventRelease()", SOURCE)


if __name__ == "__main__":
    unittest.main()
