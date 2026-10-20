import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
ADPF_HEADER = (ROOT / "src/executor/android_performance_hint.h").read_text(
    encoding="utf-8"
)
ADPF_SOURCE = (ROOT / "src/executor/android_performance_hint.cpp").read_text(
    encoding="utf-8"
)
PRESENTER = (
    ROOT
    / "funnel-arm/src/video_core/renderer_vulkan/vk_presenter.cpp"
).read_text(encoding="utf-8")
GNM_DRIVER = (
    ROOT / "funnel-arm/src/core/libraries/gnmdriver/gnmdriver.cpp"
).read_text(encoding="utf-8")


class AndroidPerformanceHintCadenceContractTests(unittest.TestCase):
    def test_ps4_present_paths_do_not_report_producer_duration(self):
        self.assertNotIn("ReportFramePresented", PRESENTER)
        self.assertNotIn("ReportProducerCycleComplete", PRESENTER)
        self.assertEqual(PRESENTER.count("MaintainSessionThreads();"), 3)

    def test_submit_done_is_the_ps4_producer_cycle_boundary(self):
        self.assertIn("void ReportProducerCycleComplete() noexcept;", ADPF_HEADER)
        submit_done = GNM_DRIVER.split(
            "int PS4_SYSV_ABI sceGnmSubmitDone()", 1
        )[1].split("int PS4_SYSV_ABI sceGnmUnmapComputeQueue", 1)[0]
        report = submit_done.index(
            "Executor::AndroidPerformanceHint::ReportProducerCycleComplete();"
        )
        self.assertLess(report, submit_done.index("HLE_TRACE;"))
        self.assertLess(report, submit_done.index("WaitGpuIdle("))

    def test_target_is_runtime_selected_and_rejects_stale_cycles(self):
        self.assertIn("FullRateProducerCycleDurationNs = 16'666'667", ADPF_SOURCE)
        self.assertIn("RelaxedProducerCycleDurationNs = 33'333'333", ADPF_SOURCE)
        self.assertIn("TargetProducerCycleDurationNs()", ADPF_SOURCE)
        self.assertIn(
            "actual_ns > MaximumReportedProducerCycleDurationNs", ADPF_SOURCE
        )
        self.assertNotIn("TargetCpuWorkDurationNs", ADPF_SOURCE)
        self.assertNotIn("WorkloadHintCooldown", ADPF_SOURCE)
        self.assertNotIn("notify_workload_increase", ADPF_SOURCE)

    def test_boundaries_are_paired_per_producer_tid_without_tls(self):
        self.assertRegex(
            ADPF_SOURCE,
            r"unordered_map<std::int32_t,\s*std::chrono::steady_clock::time_point>"
            r"\s*producer_cycle_boundaries",
        )
        self.assertIn("syscall(SYS_gettid)", ADPF_SOURCE)
        self.assertIn(
            "producer_cycle_boundaries.try_emplace(producer_tid, now)",
            ADPF_SOURCE,
        )
        report_body = ADPF_SOURCE.split(
            "void ReportProducerCycleComplete() noexcept", 1
        )[1].split("void ReportFramePresented() noexcept", 1)[0]
        self.assertNotRegex(report_body, r"\bthread_local\b")
        self.assertLess(
            report_body.index("boundary->second = now;"),
            report_body.index("actual_ns <= 0"),
        )
        self.assertNotIn("RefreshThreads(state, now)", report_body)

    def test_present_only_maintains_thread_membership(self):
        self.assertIn("void MaintainSessionThreads() noexcept;", ADPF_HEADER)
        maintenance_body = ADPF_SOURCE.split(
            "void MaintainSessionThreads() noexcept", 1
        )[1].split("void ReportProducerCycleComplete() noexcept", 1)[0]
        self.assertIn("RefreshThreads(state, now);", maintenance_body)
        self.assertNotIn("report_actual", maintenance_body)

    def test_failed_session_creation_is_rate_limited(self):
        refresh_body = ADPF_SOURCE.split(
            "void RefreshThreads(State& state,", 1
        )[1].split("#endif", 1)[0]
        self.assertIn("if (now < state.next_thread_refresh)", refresh_body)
        self.assertNotIn(
            "now < state.next_thread_refresh && state.session != nullptr",
            refresh_body,
        )
        self.assertLess(
            refresh_body.index("if (now < state.next_thread_refresh)"),
            refresh_body.index("FindCriticalThreads(state)"),
        )

    def test_unity_frame_workers_are_part_of_the_generic_adpf_session(self):
        classify = ADPF_SOURCE.split(
            "CriticalThreadRole ClassifyCriticalThread", 1
        )[1].split("std::uint64_t ReadThreadCpuTicks", 1)[0]
        self.assertIn('lowered.starts_with("unitygfx")', classify)
        self.assertIn("CriticalThreadRole::GraphicsWorker", classify)
        self.assertIn('lowered.starts_with("unityworker")', classify)

        selection = ADPF_SOURCE.split("ThreadSet FindCriticalThreads", 1)[1].split(
            "std::uint32_t ReadUclampMin", 1
        )[0]
        self.assertIn("graphics_worker_threads", selection)
        self.assertIn(
            "select_most_active(graphics_worker_threads, result, true)",
            selection,
        )

    def test_critical_thread_uclamp_is_a_monotonic_floor(self):
        scheduling = ADPF_SOURCE.split(
            "void ApplyCriticalThreadScheduling", 1
        )[1].split("void RestoreCriticalThreadScheduling", 1)[0]
        self.assertIn(
            "std::max(current_uclamp_min, CriticalThreadUclampMin)",
            scheduling,
        )
        self.assertIn("SetUclampMin(tid, target_uclamp_min)", scheduling)
        self.assertNotIn(
            "SetUclampMin(tid, CriticalThreadUclampMin)", scheduling
        )


if __name__ == "__main__":
    unittest.main()
