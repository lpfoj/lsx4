import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (
    ROOT / "funnel-arm/src/video_core/mobile_gpu_optimization.cpp"
).read_text(encoding="utf-8")


def between(start: str, end: str) -> str:
    return SOURCE.split(start, 1)[1].split(end, 1)[0]


class MobileGpuFloorHotpathContractTests(unittest.TestCase):
    def test_floor_suppression_preserves_explicit_forced_scale(self):
        helper = between(
            "bool AdaptiveScalingSuppressedByResolutionFloor() noexcept",
            "}\n\n}\n\n",
        )
        self.assertIn("physical_drs_resolution_floor.load", helper)
        self.assertIn("forced_scale_percent.load", helper)
        self.assertIn("== 0", helper)

    def test_floor_skips_inert_per_draw_and_reuse_telemetry(self):
        for start, end in (
            ("void RecordMobileGpuShadingDecision", "void RecordMobileGpuPhysicalDrsDecision"),
            ("void RecordMobileGpuSurfaceReuse", "void RecordMobileGpuResidencyGc"),
        ):
            body = between(start, end)
            gate = body.index("AdaptiveScalingSuppressedByResolutionFloor()")
            tls_or_counter = min(
                index for token in ("thread_local", "fetch_add")
                if (index := body.find(token)) >= 0
            )
            self.assertLess(gate, tls_or_counter)

    def test_floor_short_circuits_inert_vrs_decisions(self):
        for start, end in (
            ("bool MobileGpuWantsCoarseShading", "bool MobileGpuWantsPhysicalDrs"),
            ("bool MobileGpuWantsPhysicalDrs", "bool MobileGpuWantsFsr"),
        ):
            body = between(start, end)
            self.assertIn("!AdaptiveScalingSuppressedByResolutionFloor()", body)


if __name__ == "__main__":
    unittest.main()
