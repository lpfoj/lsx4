import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
HEADER = (ROOT / "funnel-arm/src/video_core/page_manager.h").read_text(
    encoding="utf-8"
)
PAGE_MANAGER = (ROOT / "funnel-arm/src/video_core/page_manager.cpp").read_text(
    encoding="utf-8"
)
STUBS = (ROOT / "funnel-arm/src/core/aerolib/stubs.cpp").read_text(
    encoding="utf-8"
)


class HleBulkWriteTlsHoistContractTests(unittest.TestCase):
    def test_same_binary_gate_is_exact_snapdragon_and_fail_closed(self):
        gate = PAGE_MANAGER.split(
            "bool ExecutorHleBulkWriteTlsHoistEnabled() noexcept", 1
        )[1].split("void BeginCurrentFexBulkLease", 1)[0]
        for required in (
            "run-preserve-hle-bulk-write-tls-lookups",
            "Common::IsSnapdragon8Gen3Host()",
            "Common::IsSnapdragon8Gen5Host()",
            "(sm8650 || sm8850) && !preserve",
            "legacyFallback=1",
            "fexLeasePhaseGrouped=1",
        ):
            self.assertIn(required, gate)

    def test_opaque_state_groups_only_bulk_fault_window_fields(self):
        self.assertIn("struct BulkGuestWriteThreadState;", HEADER)
        self.assertIn("ResolveBulkGuestWriteThreadState() noexcept", HEADER)
        state = PAGE_MANAGER.split("struct BulkGuestWriteThreadState", 1)[1].split(
            "#ifdef __ANDROID__", 1
        )[0]
        for field in ("VAddr begin{};", "VAddr end{};", "bool consumed{};"):
            self.assertIn(field, state)
        self.assertIn("ExecutorFexBulkLeasePhase fex_lease_phase", state)
        self.assertNotIn("epoch", state)
        self.assertNotIn("protection", state)


        for legacy in (
            "thread_local VAddr executor_bulk_write_begin = 0;",
            "thread_local VAddr executor_bulk_write_end = 0;",
            "thread_local bool executor_bulk_write_consumed = false;",
        ):
            self.assertIn(legacy, PAGE_MANAGER)

    def test_begin_end_preserve_phase_and_fault_semantics(self):
        begin = PAGE_MANAGER.split(
            "void BeginBulkGuestWrite(BulkGuestWriteThreadState* const state", 1
        )[1].split("void EndBulkGuestWrite(BulkGuestWriteThreadState* const state", 1)[0]
        self.assertIn("state->begin = address;", begin)
        self.assertIn("state->end =", begin)
        self.assertIn("state->consumed = false;", begin)
        self.assertIn("state->fex_lease_phase", begin)
        self.assertIn("phase == ExecutorFexBulkLeasePhase::Entered", begin)
        self.assertIn("phase = ExecutorFexBulkLeasePhase::Writing;", begin)

        fault = PAGE_MANAGER.split(
            "VAddr active_bulk_write_begin = 0;", 1
        )[1].split("VAddr completion_addr = cache_addr;", 1)[0]
        self.assertIn("ResolveBulkGuestWriteThreadState()", fault)
        self.assertIn("active_bulk_write_consumed = &state->consumed;", fault)
        self.assertIn("*active_bulk_write_consumed = true;", fault)
        self.assertIn("Common::AlignUp(active_bulk_write_end", fault)

    def test_phase_route_does_not_speculate_both_emulated_tls_objects(self):
        phase = PAGE_MANAGER.split(
            "ExecutorFexBulkLeasePhase& CurrentFexBulkLeasePhase() noexcept", 1
        )[1].split("void BeginCurrentFexBulkLease", 1)[0]
        self.assertIn("ResolveBulkGuestWriteThreadState()", phase)
        self.assertIn('asm volatile("" ::: "memory");', phase)
        self.assertIn("return state->fex_lease_phase;", phase)
        self.assertIn("return executor_fex_bulk_lease_phase;", phase)
        self.assertNotIn("executor_bulk_write_hoisted_state", phase)

        resolve = PAGE_MANAGER.split(
            "ResolveBulkGuestWriteThreadState() noexcept", 1
        )[1].split("void BeginBulkGuestWrite", 1)[0]
        self.assertIn("if (!ExecutorHleBulkWriteTlsHoistEnabled())", resolve)
        self.assertIn("return nullptr;", resolve)
        self.assertIn('asm volatile("" ::: "memory");', resolve)
        self.assertIn("return &executor_bulk_write_hoisted_state;", resolve)

    def test_hot_libc_producers_resolve_once_and_reuse_same_state(self):
        functions = (
            ("u64 ExecutorLibcMemcpy", "u64 ExecutorLibcMemmove"),
            ("u64 ExecutorLibcMemmove", "std::atomic<int> g_memset_guard_log"),
            ("u64 ExecutorLibcMemset", 'extern "C" void executor_lsx4_register_guest_gc_roots'),
        )
        for start, end in functions:
            body = STUBS.split(start, 1)[1].split(end, 1)[0]
            self.assertEqual(body.count("ResolveBulkGuestWriteThreadState()"), 1)
            self.assertIn(
                "BeginBulkGuestWrite(bulk_write_state, dest, size);", body
            )
            self.assertIn("EndBulkGuestWrite(bulk_write_state);", body)
            self.assertLess(
                body.index("BeginBulkGuestWrite(bulk_write_state"),
                body.index("PageManager::LeaveBulkGuestWrite"),
            )
            self.assertLess(
                body.index("PageManager::LeaveBulkGuestWrite"),
                body.index("EndBulkGuestWrite(bulk_write_state)"),
            )


if __name__ == "__main__":
    unittest.main()
