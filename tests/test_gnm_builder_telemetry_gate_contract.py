from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (
    ROOT / "funnel-arm/src/core/libraries/gnmdriver/gnmdriver.cpp"
).read_text(encoding="utf-8")


class GnmBuilderTelemetryGateContractTests(unittest.TestCase):
    def test_live_telemetry_is_positive_opt_in(self):
        start = SOURCE.index("static bool ExecutorLiveTelemetryEnabled()")
        end = SOURCE.index("static bool ExecutorLiveShouldPulse", start)
        gate = SOURCE[start:end]
        self.assertIn("EXECUTOR_LIVE_TELEMETRY", gate)
        self.assertIn("run-diagnostic-gnm-live-telemetry", gate)
        self.assertIn("ExecutorTraceLiveWide()", gate)
        self.assertNotIn('getenv("EXECUTOR_LIVE_QUIET")', gate)

    def test_production_keeps_fallback_bounds_and_skips_live_statistics(self):
        start = SOURCE.index("static void ExecutorLiveGnmBuilderLog(")
        end = SOURCE.index("static void ExecutorLiveGnmUnknownLog(", start)
        body = SOURCE[start:end]

        gate = body.index("if (!collect_live_telemetry)", body.index("g_live_active_cmdbuf.store"))
        fast_exit = body.index("if (!collect_live_telemetry &&")
        self.assertLess(fast_exit, body.index("std::strcmp"))
        self.assertLess(fast_exit, body.index("std::strstr"))
        self.assertIn("g_live_submit_calls.load(std::memory_order_relaxed) != 0", body)
        self.assertLess(body.index("g_live_active_cmdbuf.store"), gate)
        self.assertLess(body.index("g_live_active_cmdbuf_end.compare_exchange_weak"), gate)

        diagnostics = body[gate:]
        self.assertIn("ExecutorLiveGnmApiIsActualDraw", diagnostics)
        self.assertIn("g_live_gnm_builder_total.fetch_add", diagnostics)
        self.assertIn("ExecutorLiveMaybeStartPostShaderWatchdog", diagnostics)
        self.assertIn("first_builder_log_budget", diagnostics)

    def test_delayed_sync_frontier_dump_is_live_telemetry_only(self):
        start = SOURCE.index("static std::atomic<bool> sync_frontier_dump_started")
        end = SOURCE.index("EXECUTOR_LIVE_STALL_THREAD_DUMP", start)
        body = SOURCE[start:end]
        self.assertIn("if (ExecutorLiveTelemetryEnabled() &&", body)

    def test_flip_logging_and_counter_are_live_trace_only_on_android(self):
        patch_start = SOURCE.index("static inline s32 PatchFlipRequest(")
        submit_start = SOURCE.index(
            "s32 PS4_SYSV_ABI sceGnmSubmitAndFlipCommandBuffersForWorkload(",
            patch_start,
        )
        patch = SOURCE[patch_start:submit_start]
        counter = patch.index("patched_flip_write_log_count")
        self.assertIn("if (ExecutorTraceLiveWide())", patch[max(0, counter - 160):counter])

        submit_end = SOURCE.index("bool ExecutorEnsureLiverpoolForReplay()", submit_start)
        submit = SOURCE[submit_start:submit_end]
        trace = submit.index("if (ExecutorTraceLiveWide())")
        attempt = submit.index("EXECUTOR_GNM_FLIP_ATTEMPT")
        flip = submit.index("EXECUTOR_GNM_FLIP api=")
        desktop = submit.index("#else", trace)
        self.assertLess(trace, attempt)
        self.assertLess(flip, desktop)

    def test_marker_detail_formatting_is_skipped_without_live_telemetry(self):
        for signature, endpoint in (
            ("sceGnmInsertPushColorMarker(", "sceGnmInsertPushMarker("),
            ("sceGnmInsertPushMarker(", "sceGnmInsertSetColorMarker("),
            ("sceGnmInsertSetMarker(", "sceGnmInsertThreadTraceMarker("),
            ("sceGnmInsertWaitFlipDone(", "sceGnmIsCoredumpValid("),
        ):
            start = SOURCE.index(signature)
            next_function = SOURCE.index(endpoint, start + len(signature))
            body = SOURCE[start:next_function]
            snprintf = body.rfind("std::snprintf")
            if snprintf != -1:
                gate = body.rfind("if (ExecutorLiveTelemetryEnabled())", 0, snprintf)
                self.assertNotEqual(gate, -1, signature)
                self.assertLess(gate, snprintf, signature)

    def test_launch_time_environment_switches_are_cached(self):
        wait_start = SOURCE.index("sceGnmInsertWaitFlipDone(")
        wait_end = SOURCE.index("sceGnmIsCoredumpValid", wait_start)
        wait = SOURCE[wait_start:wait_end]
        self.assertIn("static const bool force_flip_done", wait)

        pa_start = SOURCE.index("sceGnmIsUserPaEnabled(")
        pa_end = SOURCE.index("sceGnmLogicalCuIndexToPhysicalCuIndex", pa_start)
        pa = SOURCE[pa_start:pa_end]
        self.assertIn("static const bool enabled", pa)


if __name__ == "__main__":
    unittest.main()
