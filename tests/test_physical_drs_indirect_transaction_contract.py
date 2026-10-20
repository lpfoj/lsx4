from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (
    ROOT
    / "funnel-arm"
    / "src"
    / "video_core"
    / "renderer_vulkan"
    / "vk_rasterizer.cpp"
).read_text(encoding="utf-8")


class PhysicalDrsIndirectTransactionContract(unittest.TestCase):
    def test_indirect_draw_keeps_selection_result(self) -> None:
        body = SOURCE.split("void Rasterizer::DrawIndirect", 1)[1].split(
            "void Rasterizer::DispatchDirect", 1
        )[0]
        self.assertIn(
            "const bool executor_physical_drs_selected =\n"
            "        ExecutorBeginPhysicalDrs(pipeline, state);",
            body,
        )

    def test_stale_scaled_attachment_is_rejected_before_render_scope(self) -> None:
        body = SOURCE.split("void Rasterizer::DrawIndirect", 1)[1].split(
            "void Rasterizer::DispatchDirect", 1
        )[0]
        guard = body.index("if (executor_physical_drs_selected)")
        active = body.index("if (!executor_physical_drs_draw_active)", guard)
        reset = body.index("ResetBindings();", active)
        reject = body.index("return;", reset)
        begin = body.index("scheduler.BeginRendering(state);", reject)
        self.assertLess(guard, active)
        self.assertLess(active, reset)
        self.assertLess(reset, reject)
        self.assertLess(reject, begin)


if __name__ == "__main__":
    unittest.main()
