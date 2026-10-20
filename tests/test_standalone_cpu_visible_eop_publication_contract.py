import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
LIVERPOOL = (
    ROOT / "funnel-arm/src/video_core/amdgpu/liverpool.cpp"
).read_text(encoding="utf-8")
LIVERPOOL_HEADER = (
    ROOT / "funnel-arm/src/video_core/amdgpu/liverpool.h"
).read_text(encoding="utf-8")


class StandaloneCpuVisibleEopPublicationContract(unittest.TestCase):
    def test_cpu_visible_eop_is_recorded_for_root_submission_decision(self):
        eop = LIVERPOOL.split("case PM4ItOpcode::EventWriteEop:", 1)[1].split(
            "case PM4ItOpcode::DmaData:", 1
        )[0]

        self.assertIn("cpu_visible_graphics_eop_queued", eop)
        self.assertIn("DataSelect::Data32Low", eop)
        self.assertIn("DataSelect::Data64", eop)
        self.assertNotIn("FlushGpuCompletionBatchForExecutor", eop)

    def test_only_root_completion_only_submission_forces_publication(self):
        root_tail = LIVERPOOL.split("const bool completion_only_eop_submit", 1)[1].split(
            "ExecutorEopTraceParserEnd", 1
        )[0]

        self.assertIn("ib_depth == 0", root_tail)
        self.assertIn("!state->abort", root_tail)
        self.assertIn("state->cpu_visible_graphics_eop_queued", root_tail)
        self.assertIn("!state->graphics_gpu_work_recorded", root_tail)
        self.assertIn(
            "FlushGpuCompletionBatchForExecutor(true, false)", root_tail
        )

    def test_real_gpu_work_is_shared_across_indirect_buffers(self):
        self.assertIn("bool graphics_gpu_work_recorded{}", LIVERPOOL_HEADER)
        self.assertIn("bool cpu_visible_graphics_eop_queued{}", LIVERPOOL_HEADER)

        work_cases = [
            "DrawIndex2",
            "DrawIndexOffset2",
            "DrawIndexAuto",
            "DrawIndirect",
            "DrawIndirectMulti",
            "DrawIndexIndirect",
            "DrawIndexIndirectMulti",
            "DrawIndexIndirectCountMulti",
            "DispatchDirect",
            "DispatchIndirect",
            "DmaData",
        ]
        execution = LIVERPOOL.split("\n            case PM4ItOpcode::Nop: {", 1)[1]
        for opcode in work_cases:
            section = execution.split(f"case PM4ItOpcode::{opcode}:", 1)[1].split(
                "case PM4ItOpcode::", 1
            )[0]
            self.assertIn("state->graphics_gpu_work_recorded = true", section, opcode)

        flip = LIVERPOOL.split("case PM4CmdNop::PayloadType::PatchedFlip:", 1)[1].split(
            "case PM4CmdNop::PayloadType::", 1
        )[0]
        self.assertIn("state->graphics_gpu_work_recorded = true", flip)

    def test_render_submit_batching_remains_enabled(self):
        tail = LIVERPOOL.split("if (submit_done)", 1)[1].split(
            "Liverpool::Task Liverpool::ProcessCeUpdate", 1
        )[0]
        self.assertIn("ExecutorBoundedRasterizerPipeline()", tail)
        self.assertIn(
            "rasterizer->MaximumDeferredGuestSubmitBoundaries()", tail
        )


if __name__ == "__main__":
    unittest.main()
