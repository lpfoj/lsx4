import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PAGE_MANAGER = (
    ROOT / "funnel-arm/src/video_core/page_manager.cpp"
).read_text(encoding="utf-8")


class Sm8650FastBulkWriteHazardContractTests(unittest.TestCase):
    def test_regular_d51_publishes_hazard_but_still_elides_redundant_pte(self):
        body = PAGE_MANAGER.split(
            "void SetBulkGuestWriteActivity", 1
        )[1].split("static bool GuestFaultSignalHandler", 1)[0]
        self.assertIn("Common::IsSnapdragon8Gen3Host()", body)
        self.assertIn("publish_sm8650_write_hazard", body)
        self.assertIn("elide_redundant_pte_transition", body)
        self.assertIn("SetWriteFaultBypass(page, true);", body)
        self.assertIn("if (!elide_redundant_pte_transition)", body)
        self.assertIn("SetWriteFaultBypass(page, false);", body)
        self.assertLess(
            body.index("SetWriteFaultBypass(page, true);"),
            body.index("Protect(static_cast<VAddr>(page) << PAGE_BITS"),
        )

    def test_fused_d98_publishes_same_hazard_before_guest_memcpy(self):
        body = PAGE_MANAGER.split(
            "bool TryBeginFastBulkGuestWrite", 1
        )[1].split("void CancelPreparedBulkGuestWrite", 1)[0]
        self.assertIn("Common::IsSnapdragon8Gen3Host()", body)
        self.assertIn("BulkWriteActive(page)", body)
        self.assertIn("FexDeferredWriteBypassed(page)", body)
        self.assertIn(
            "publish_sm8650_write_hazard &&\n"
            "                 (BulkWriteActive(page) || FexDeferredWriteBypassed(page))",
            body,
        )
        self.assertIn("UpdateBulkWriterCount(page, true)", body)
        self.assertIn(
            "ASSERT_MSG(!publish_sm8650_write_hazard || writer_count == 1",
            body,
        )
        self.assertIn("SetWriteFaultBypass(page, true);", body)
        self.assertLess(
            body.index("BulkWriteActive(page)"),
            body.index("UpdateBulkWriterCount(page, true)"),
        )
        self.assertLess(
            body.index("FexDeferredWriteBypassed(page)"),
            body.index("UpdateBulkWriterCount(page, true)"),
        )
        self.assertLess(
            body.index("UpdateBulkWriterCount(page, true)"),
            body.index("SetWriteFaultBypass(page, true);"),
        )

    def test_a840_measured_fast_route_is_not_changed(self):
        regular = PAGE_MANAGER.split(
            "void SetBulkGuestWriteActivity", 1
        )[1].split("static bool GuestFaultSignalHandler", 1)[0]
        self.assertIn(
            "elide_redundant_pte_transition &&\n"
            "                            !publish_sm8650_write_hazard",
            regular,
        )


if __name__ == "__main__":
    unittest.main()
