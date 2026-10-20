from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
TRACE_CPP = (
    ROOT / "funnel-arm/src/video_core/invalidation_overlap_trace.cpp"
).read_text(encoding="utf-8")
TRACE_HEADER = (
    ROOT / "funnel-arm/src/video_core/invalidation_overlap_trace.h"
).read_text(encoding="utf-8")
RASTERIZER = (
    ROOT / "funnel-arm/src/video_core/renderer_vulkan/vk_rasterizer.cpp"
).read_text(encoding="utf-8")
NATIVE = (ROOT / "src/executor/native_runtime_api.cpp").read_text(
    encoding="utf-8"
)
DRIVER = (ROOT / "tools/codex_bloodborne_pmu.sh").read_text(
    encoding="utf-8"
)


def _between(source, start, end):
    return source.split(start, 1)[1].split(end, 1)[0]


RECORD_WAIT = _between(
    TRACE_HEADER,
    "inline void RecordWaitSpan(",
    "\n}\n\n// Enabled-only DRAIN recorder",
)
HOT_RECORD = _between(
    TRACE_CPP, "void RecordEnabled(", "\nstd::uint64_t ReservedEntries("
)
ACQUIRE_RECORD = _between(
    TRACE_CPP, "TraceRecord* AcquireRecord(", "\nvoid RecordEnabled("
)
DRAIN_WRAPPER = _between(
    RASTERIZER,
    "void Rasterizer::DrainGuestInvalidations() {",
    "\nvoid Rasterizer::DrainGuestInvalidationsImpl() {",
)
WAIT_FINISH = _between(
    RASTERIZER,
    "    if (executor_guest_invalidation_timing_enabled) {",
    "#endif\n    return request.result;",
)


class InvalidationOverlapTraceContractTests(unittest.TestCase):
    def test_marker_activation_is_early_and_mutually_exclusive(self):
        self.assertIn("run-profile-guest-invalidation-overlap", NATIVE)
        self.assertIn("EXECUTOR_PROFILE_GUEST_INVALIDATION_OVERLAP", NATIVE)
        self.assertIn("reason=source-profiler-conflict", NATIVE)
        self.assertIn("!guest_invalidation_source_profile_requested", NATIVE)
        self.assertIn("InvalidationOverlapTrace::Initialize();", RASTERIZER)
        self.assertLess(
            RASTERIZER.index("InvalidationOverlapTrace::Initialize();"),
            RASTERIZER.index("memory->SetRasterizer(this);"),
        )

    def test_driver_cannot_enable_both_profiles(self):
        self.assertIn("BB_PROFILE_INVALIDATION_OVERLAP", DRIVER)
        self.assertIn("INVALID_PROFILE_COMBINATION", DRIVER)
        self.assertIn("run-profile-guest-invalidation-overlap", DRIVER)
        self.assertIn("invalidation-overlap-control", DRIVER)
        self.assertIn("OVERLAP_READY", DRIVER)
        self.assertIn("OVERLAP_DONE", DRIVER)
        self.assertIn("invalidation-overlap.bin", DRIVER)
        self.assertIn("parse_invalidation_overlap_trace.py", DRIVER)
        self.assertLess(
            DRIVER.index("OVERLAP_DONE="),
            DRIVER.rindex("stop_game || exit 65\ntrap - EXIT"),
        )

    def test_driver_wraps_every_adb_action_and_cleans_markers(self):
        device_guard = _between(
            DRIVER, "guard_device_action() {", "\n}\n\nadb_device()"
        )
        foreground_wrapper = _between(
            DRIVER, "adb_device() {", "\n}\n\nadb_device_background()"
        )
        background_wrapper = _between(
            DRIVER, "adb_device_background() {", "\n}\n\nhost_path()"
        )
        self.assertIn("agentchat.py", DRIVER)
        self.assertIn('--room "$agent_room"', device_guard)
        self.assertIn('--target "$agent_target"', device_guard)
        self.assertIn('--area "$agent_target"', device_guard)
        self.assertIn("event --agent codex", device_guard)
        self.assertLess(device_guard.index(" event "), device_guard.index(" own "))
        for wrapper in (foreground_wrapper, background_wrapper):
            self.assertEqual(wrapper.count("guard_device_action"), 1)
            self.assertEqual(wrapper.count("adb -s \"$device\""), 1)
            self.assertLess(
                wrapper.index("guard_device_action"),
                wrapper.index("adb -s \"$device\""),
            )
        self.assertEqual(DRIVER.count("adb -s \"$device\""), 2)
        cleanup = _between(DRIVER, "cleanup_profile_markers() {", "\n}\n\nwait_device_file()")
        for marker in ("arm", "ready", "stop", "done", "error", "trace-v1.bin"):
            self.assertIn(marker, cleanup)

    def test_default_wait_hook_is_one_pointer_gate(self):
        self.assertEqual(RECORD_WAIT.count("ActiveContext.load("), 1)
        self.assertEqual(RECORD_WAIT.count("[[unlikely]]"), 1)
        for forbidden in (
            "thread_writer",
            "steady_clock::now",
            "mutex",
            "malloc",
            "new ",
            "fopen",
            "write(",
            "msync",
            "sort",
        ):
            self.assertNotIn(forbidden, RECORD_WAIT)

    def test_wait_clocks_are_entry_gated_and_reused_by_trace(self):
        wait_start = _between(
            RASTERIZER,
            "const bool executor_guest_invalidation_overlap_enabled =",
            "GuestInvalidationRequest request{",
        )
        self.assertIn("InvalidationOverlapTrace::Enabled()", wait_start)
        self.assertIn(
            "executor_guest_invalidation_profile_enabled ||\n"
            "        executor_guest_invalidation_overlap_enabled",
            wait_start,
        )
        self.assertIn(
            "executor_guest_invalidation_timing_enabled\n"
            "                                ? std::chrono::steady_clock::now()",
            wait_start,
        )
        self.assertIn("wait_end - wait_begin", WAIT_FINISH)
        self.assertIn(
            "RecordWaitSpan(\n                wait_begin, wait_end", WAIT_FINISH
        )
        self.assertIn(
            "if (executor_guest_invalidation_overlap_enabled)", WAIT_FINISH
        )
        self.assertNotIn("steady_clock::now() - wait_begin", WAIT_FINISH)

    def test_default_drain_hook_gates_before_clocks(self):
        gate = "if (VideoCore::InvalidationOverlapTrace::Enabled()) [[unlikely]]"
        self.assertIn(gate, DRAIN_WRAPPER)
        self.assertLess(DRAIN_WRAPPER.index(gate), DRAIN_WRAPPER.index("steady_clock::now"))
        self.assertEqual(DRAIN_WRAPPER.count("InvalidationOverlapTrace::Enabled()"), 1)
        self.assertIn("DrainGuestInvalidationsImpl();", DRAIN_WRAPPER)

    def test_enabled_recorder_uses_global_reservation_without_emulated_tls(self):
        for required in (
            "AcquireRecord(storage)",
            "next_entry.fetch_add",
            "commit.store",
            "std::memory_order_release",
        ):
            self.assertIn(required, HOT_RECORD + ACQUIRE_RECORD)
        self.assertNotIn("thread_local", TRACE_CPP)
        self.assertNotIn("ThreadWriter", TRACE_CPP)
        for forbidden in (
            "mutex",
            "vector",
            "malloc",
            "new ",
            "fopen",
            "write(",
            "msync",
            "sort",
            "steady_clock::now",
        ):
            self.assertNotIn(forbidden, HOT_RECORD + ACQUIRE_RECORD)

    def test_nonwrapping_capacity_and_saturation_disable_recording(self):
        self.assertIn("TraceCapacityEntries = 1u << 19", TRACE_CPP)
        self.assertIn("index >= TraceCapacityEntries", ACQUIRE_RECORD)
        self.assertIn("Saturate(storage)", ACQUIRE_RECORD)
        self.assertIn("coverage_exact.store(0", TRACE_CPP)
        self.assertIn("ActiveContext.compare_exchange_strong", TRACE_CPP)
        self.assertIn("dropped.store(1", TRACE_CPP)
        self.assertNotIn("dropped.fetch_add", TRACE_CPP)
        self.assertNotIn("% TraceCapacityEntries", ACQUIRE_RECORD)

    def test_trace_is_prefaulted_only_after_arm_before_ready(self):
        controller = _between(
            TRACE_CPP,
            "void ControllerMain(const std::string& control_directory) {",
            "\n#endif",
        )
        initialize = _between(
            TRACE_CPP, "void Initialize() noexcept {", "\nvoid Detail::RecordWaitSpanEnabled"
        )
        arm_wait = "while (!PathExists(arm_path))"
        prefault = "ResetAndPrefault(trace_storage)"
        publish = "ActiveContext.store(&trace_storage"
        ready = "std::rename(ready_temporary_path.c_str(), ready_path.c_str())"
        for token in (arm_wait, prefault, publish, ready):
            self.assertIn(token, controller)
        self.assertLess(controller.index(arm_wait), controller.index(prefault))
        self.assertLess(controller.index(prefault), controller.index(publish))
        self.assertLess(controller.index(publish), controller.index(ready))
        self.assertNotIn(prefault, initialize)
        self.assertNotIn(publish, initialize)

    def test_stop_quiesces_writers_before_self_dump(self):
        quiesce = _between(TRACE_CPP, "void Quiesce(", "\n\n#ifdef __ANDROID__")
        controller = _between(
            TRACE_CPP,
            "void ControllerMain(const std::string& control_directory) {",
            "\n#endif",
        )
        self.assertIn("ActiveContext.exchange(nullptr", quiesce)
        self.assertIn("in_flight_writers.load", quiesce)
        self.assertIn("quiesced.store(1", quiesce)
        self.assertIn("PathExists(stop_path)", controller)
        self.assertLess(controller.index("Quiesce(trace_storage"), controller.index("DumpQuiescedTrace"))
        self.assertLess(controller.index("DumpQuiescedTrace"), controller.rindex("WriteSmallFile(done_path"))

    def test_live_snapshot_refuses_to_advertise_exact_coverage(self):
        snapshot = _between(
            TRACE_CPP,
            'extern "C" std::uint64_t executor_funnel_invalidation_overlap_trace_snapshot_v1(',
            "\n}",
        )
        self.assertIn("quiesced.load", snapshot)
        self.assertIn("ActiveContext.load", snapshot)
        self.assertIn("return 0", snapshot)
        self.assertIn("!enabled && quiesced", TRACE_CPP)

    def test_v1_abi_refuses_to_claim_epoch_mapping(self):
        self.assertIn("epoch_mapping_exact{}", TRACE_HEADER)
        self.assertIn("FeatureFrameBoundaries", TRACE_HEADER)
        self.assertIn("FeaturePresentMapping", TRACE_HEADER)
        self.assertIn("epochMappingExact=0", TRACE_CPP)
        self.assertIn("TraceFeatures = FeatureWaitSpans | FeatureDrainSpans", TRACE_CPP)
        self.assertIn("trace_stats_v1", TRACE_HEADER)
        self.assertIn("trace_snapshot_v1", TRACE_HEADER)


if __name__ == "__main__":
    unittest.main()
