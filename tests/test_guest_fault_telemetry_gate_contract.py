from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (
    ROOT / "funnel-arm/src/video_core/page_manager.cpp"
).read_text(encoding="utf-8")


class GuestFaultTelemetryGateContractTests(unittest.TestCase):
    def test_fault_repair_precedes_and_is_independent_of_live_counters(self):
        start = SOURCE.index("static bool GuestFaultSignalHandler(")
        end = SOURCE.index("void PageManager::CompleteInitialGpuMappingRegistration", start)
        body = SOURCE[start:end]
        classify = body.index("owner->ClassifyAndRepairFault(")
        gate = body.index("if (!VideoCore::IsExecutorLiveQuiet())", classify)
        repaired = body.index("if (ownership == FaultOwnership::Repaired)", gate)
        self.assertLess(classify, gate)
        self.assertLess(gate, repaired)

        counter_block = body[gate:repaired]
        self.assertIn("executor_owned_generated_fault_count.fetch_add", counter_block)
        self.assertIn("executor_owned_host_fault_count.fetch_add", counter_block)
        self.assertIn("executor_owned_unclassified_fault_count.fetch_add", counter_block)
        self.assertIn("[EXECUTOR_GENERATED_WRITE_FAULTS]", counter_block)

        functional_tail = body[repaired:]
        self.assertIn("ReserveFexWriteEpoch", functional_tail)
        self.assertIn("ConsumeMemoryWaitWriteFault", functional_tail)

    def test_gpu_thread_fault_tid_syscall_and_log_are_diagnostic_only(self):
        start = SOURCE.index("static bool GuestFaultSignalHandler(")
        end = SOURCE.index("void PageManager::CompleteInitialGpuMappingRegistration", start)
        body = SOURCE[start:end]
        tid = body.index("const int fault_tid = static_cast<int>(gettid())")
        gate = body.rfind("if (!VideoCore::IsExecutorLiveQuiet())", 0, tid)
        self.assertNotEqual(gate, -1)
        self.assertLess(gate, tid)
        previous_state = body.index("const bool previous_fault_state", tid)
        diagnostics = body[gate:previous_state]
        self.assertIn("executor_gpu_thread_host_fault_count.fetch_add", diagnostics)
        self.assertIn("[EXECUTOR_GPU_THREAD_PAGE_FAULT]", diagnostics)

        functional_tail = body[previous_state:]
        self.assertIn("executor_handling_guest_fault = true", functional_tail)
        self.assertIn("ConsumeMemoryWaitWriteFault", functional_tail)


if __name__ == "__main__":
    unittest.main()
