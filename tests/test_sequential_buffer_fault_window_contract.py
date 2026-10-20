import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PAGE_MANAGER = (
    ROOT / "funnel-arm" / "src" / "video_core" / "page_manager.cpp"
).read_text(encoding="utf-8")


class ExactPageWriteFaultContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.fault = PAGE_MANAGER.split(
            "static bool GuestFaultSignalHandler", 1
        )[1].split("return handled ||", 1)[0]

    def test_ordinary_write_fault_stays_on_the_faulting_page(self):
        setup = self.fault.split("const bool cache_event", 1)[0]
        self.assertIn("u64 cache_size = PAGE_SIZE", setup)
        self.assertNotIn("SequentialGeneratedBufferWriteFaultSpan", setup)
        self.assertNotIn("MaxWindowPages", setup)
        self.assertIn("ordinary fault owns only the faulting 4 KiB page", setup)

    def test_only_explicit_bulk_contract_can_widen_invalidation(self):
        explicit = self.fault.index("executor_bulk_write_begin")
        widen = self.fault.index("executor_bulk_write_end, PAGE_SIZE")
        invalidate = self.fault.index("InvalidateMemoryFromGuestThread")
        release = self.fault.index("ReleaseDeferredWriteProtectionRange")
        self.assertLess(explicit, widen)
        self.assertLess(widen, invalidate)
        self.assertLess(invalidate, release)
        self.assertIn("executor_bulk_write_consumed = true", self.fault)
        self.assertEqual(
            self.fault.split("VAddr cache_addr", 1)[1]
            .split("const bool cache_event", 1)[0]
            .count("cache_size ="),
            2,
        )


if __name__ == "__main__":
    unittest.main()
