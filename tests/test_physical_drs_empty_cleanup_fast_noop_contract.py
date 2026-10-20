from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
VIDEO = ROOT / "funnel-arm" / "src" / "video_core" / "renderer_vulkan"
HEADER = (VIDEO / "vk_rasterizer.h").read_text(encoding="utf-8")
SOURCE = (VIDEO / "vk_rasterizer.cpp").read_text(encoding="utf-8")


class PhysicalDrsEmptyCleanupFastNoopContract(unittest.TestCase):
    def test_fast_noop_is_bloodborne_marker_reversible(self) -> None:
        body = SOURCE.split(
            "void Rasterizer::ExecutorPreparePhysicalDrsDraw", 1
        )[1].split(
            "void Rasterizer::ExecutorTransitionPhysicalDrsSurface", 1
        )[0]
        helper = SOURCE.split(
            "static bool ExecutorBloodborneEmptyPhysicalDrsCleanupFastNoop", 1
        )[1].split("constexpr bool ExecutorDmaCleanEpochWouldSkip", 1)[0]
        self.assertIn("run-preserve-empty-physical-drs-draw-cleanup", helper)
        self.assertIn('MemoryPatcher::g_game_serial == "CUSA03173"', helper)
        self.assertIn("!managed_mobile_gpu", body)
        self.assertIn(
            "ExecutorBloodborneEmptyPhysicalDrsCleanupFastNoop()", body
        )

    def test_false_residency_proof_returns_before_recursive_mutex(self) -> None:
        body = SOURCE.split(
            "void Rasterizer::ExecutorPreparePhysicalDrsDraw", 1
        )[1].split(
            "void Rasterizer::ExecutorTransitionPhysicalDrsSurface", 1
        )[0]
        proof = "!executor_physical_drs_has_residency.load("
        lock = "std::scoped_lock drs_lock{executor_physical_drs_mutex};"
        self.assertIn(proof, body)
        self.assertLess(body.index(proof), body.index(lock))
        fast = body[body.index(proof) : body.index(lock)]
        self.assertIn("return;", fast)

    def test_empty_resolve_and_submit_also_avoid_cleanup_transactions(self) -> None:
        resolve = SOURCE.split(
            "bool Rasterizer::ExecutorResolvePhysicalDrs", 1
        )[1].split("bool Rasterizer::ExecutorAcquirePhysicalDrsPresentationSource", 1)[0]
        self.assertIn(
            "ExecutorBloodborneEmptyPhysicalDrsCleanupFastNoop()", resolve
        )
        self.assertIn("!executor_physical_drs_has_residency.load(", resolve)
        self.assertLess(
            resolve.index("return false;"),
            resolve.index("std::scoped_lock drs_lock{executor_physical_drs_mutex};"),
        )

        submit = SOURCE.split("void Rasterizer::OnSubmit", 1)[1].split(
            "void Rasterizer::DisableFexDeferredWriteEpochsForPhysicalAliasing", 1
        )[0]
        self.assertIn("skip_empty_physical_drs_submit_cleanup", submit)
        self.assertIn(
            "ExecutorBloodborneEmptyPhysicalDrsCleanupFastNoop()", submit
        )
        self.assertIn("if (!skip_empty_physical_drs_submit_cleanup)", submit)

    def test_residency_witness_is_atomic_and_published_at_every_emplace(self) -> None:
        self.assertIn(
            "std::atomic<bool> executor_physical_drs_has_residency{false};",
            HEADER,
        )
        self.assertEqual(SOURCE.count("executor_physical_drs_surfaces.emplace_back"), 2)
        self.assertEqual(
            SOURCE.count("executor_physical_drs_depth_surfaces.emplace_back"), 1
        )
        for fragment in SOURCE.split("executor_physical_drs_surfaces.emplace_back")[1:]:
            publish = fragment[:420]
            self.assertIn("executor_physical_drs_has_residency.store(", publish)
            self.assertIn("true, std::memory_order_release", publish)
        depth = SOURCE.split(
            "executor_physical_drs_depth_surfaces.emplace_back", 1
        )[1][:420]
        self.assertIn("executor_physical_drs_has_residency.store(", depth)
        self.assertIn("true, std::memory_order_release", depth)

    def test_gc_clears_witness_only_after_both_lists_are_empty(self) -> None:
        gc = SOURCE.split(
            "bool Rasterizer::ExecutorRunPhysicalDrsResidencyGc", 1
        )[1].split(
            "bool Rasterizer::ExecutorPhysicalDrsPipelineCompatible", 1
        )[0]
        clear = gc.index(
            "executor_physical_drs_has_residency.store(false,"
        )
        window = gc[max(0, clear - 240) : clear]
        self.assertIn("executor_physical_drs_surfaces.empty()", window)
        self.assertIn("executor_physical_drs_depth_surfaces.empty()", window)
        self.assertIn("std::memory_order_release", gc[clear : clear + 180])


if __name__ == "__main__":
    unittest.main()
