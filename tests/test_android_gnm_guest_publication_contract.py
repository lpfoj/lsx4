import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
GNM = (
    ROOT
    / "funnel-arm"
    / "src"
    / "core"
    / "libraries"
    / "gnmdriver"
    / "gnmdriver.cpp"
).read_text(encoding="utf-8")
LIVERPOOL = (
    ROOT / "funnel-arm" / "src" / "video_core" / "amdgpu" / "liverpool.cpp"
).read_text(encoding="utf-8")


def before_hle(function_signature: str) -> str:
    return GNM.split(function_signature, 1)[1].split("HLE_TRACE", 1)[0]


class AndroidGnmGuestPublicationContractTest(unittest.TestCase):
    def test_graphics_submission_publishes_once_before_native_reads(self):
        prefix = before_hle("sceGnmSubmitCommandBuffersForWorkload(")
        self.assertEqual(prefix.count("atomic_thread_fence"), 1)
        self.assertIn("std::memory_order_acq_rel", prefix)
        for forbidden in ("WaitGpuIdle", "WaitRasterizer", "mutex", "lock_guard", "CUSA"):
            self.assertNotIn(forbidden, prefix)

    def test_compute_submission_publishes_once_before_either_ring_segment(self):
        body = GNM.split("sceGnmDingDong(u32 gnm_vqid", 1)[1].split(
            "sceGnmDingDongForWorkload", 1
        )[0]
        self.assertEqual(body.count("atomic_thread_fence"), 1)
        fence = body.index("atomic_thread_fence(std::memory_order_acq_rel)")
        first_submit = body.index("liverpool->SubmitAsc")
        self.assertLess(fence, first_submit)
        self.assertEqual(body.count("SubmitAsc("), 2)

    def test_internal_submit_loops_do_not_repeat_the_publication_fence(self):
        graphics = LIVERPOOL.split("void Liverpool::SubmitGfx", 1)[1].split(
            "void Liverpool::SubmitAsc", 1
        )[0]
        compute = LIVERPOOL.split("void Liverpool::SubmitAsc", 1)[1]
        self.assertNotIn("atomic_thread_fence", graphics)
        self.assertNotIn("atomic_thread_fence", compute)

    def test_submit_done_pays_barrier_only_for_reachable_non_jit_fallback(self):
        submit_done = GNM.split("int PS4_SYSV_ABI sceGnmSubmitDone()", 1)[1]
        fallback = submit_done.split("if (!suppress_active_dcb", 1)[1].split(
            "if (!WaitGpuIdle", 1
        )[0]
        self.assertEqual(fallback.count("atomic_thread_fence"), 1)
        fence = fallback.index("atomic_thread_fence(std::memory_order_acq_rel)")
        submit = fallback.index("liverpool->SubmitGfx")
        self.assertLess(fence, submit)


if __name__ == "__main__":
    unittest.main()
