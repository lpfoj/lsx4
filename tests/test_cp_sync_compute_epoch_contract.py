import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
RASTERIZER_CPP = (
    ROOT / "funnel-arm/src/video_core/renderer_vulkan/vk_rasterizer.cpp"
).read_text(encoding="utf-8")
RASTERIZER_H = (
    ROOT / "funnel-arm/src/video_core/renderer_vulkan/vk_rasterizer.h"
).read_text(encoding="utf-8")
TEXTURE_CACHE_H = (
    ROOT / "funnel-arm/src/video_core/texture_cache/texture_cache.h"
).read_text(encoding="utf-8")


def function_body(source: str, signature: str) -> str:
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 0
    for pos in range(brace, len(source)):
        if source[pos] == "{":
            depth += 1
        elif source[pos] == "}":
            depth -= 1
            if depth == 0:
                return source[brace + 1 : pos]
    raise AssertionError(f"unterminated function: {signature}")


class CpSyncComputeEpochContractTests(unittest.TestCase):
    def test_dirty_bit_is_instance_local_and_android_only(self):
        android_section = RASTERIZER_H.split("#ifdef __ANDROID__", 1)[1]
        self.assertIn("bool executor_compute_work_since_cp_sync{};", android_section)

    def test_cp_sync_preserves_transfer_flush_and_legacy_barrier(self):
        body = function_body(RASTERIZER_CPP, "void Rasterizer::CpSync()")
        flush = body.index("scheduler.FlushPendingBufferTransfers();")
        gate = body.index("if (!executor_compute_work_since_cp_sync)")
        end_render = body.index("scheduler.EndRendering(")
        barrier = body.index("cmdbuf.pipelineBarrier(")
        self.assertLess(flush, gate)
        self.assertLess(gate, end_render)
        self.assertLess(end_render, barrier)
        self.assertIn("VideoCore::IsManagedVulkanDriverOptimization()", body)
        self.assertIn("executor_compute_work_since_cp_sync = false;", body)
        self.assertIn("vk::PipelineStageFlagBits::eComputeShader", body)
        self.assertIn("vk::PipelineStageFlagBits::eDrawIndirect", body)

    def test_dirty_bit_is_set_only_after_recorded_dispatches(self):
        direct = function_body(RASTERIZER_CPP, "void Rasterizer::DispatchDirect()")
        indirect = function_body(RASTERIZER_CPP, "void Rasterizer::DispatchIndirect(")
        direct_dispatch = direct.index("cmdbuf.dispatch(")
        direct_dirty = direct.index("executor_compute_work_since_cp_sync = true;")
        indirect_dispatch = indirect.index("cmdbuf.dispatchIndirect(")
        indirect_dirty = indirect.index("executor_compute_work_since_cp_sync = true;")
        self.assertLess(direct_dispatch, direct_dirty)
        self.assertLess(indirect_dispatch, indirect_dirty)
        self.assertNotIn(
            "executor_compute_work_since_cp_sync = true;",
            direct[:direct_dispatch],
        )
        self.assertNotIn(
            "executor_compute_work_since_cp_sync = true;",
            indirect[:indirect_dispatch],
        )

    def test_indirect_consumers_keep_exact_resource_barriers(self):
        exact_barrier = re.compile(
            r"GetBarrier\(vk::AccessFlagBits2::eIndirectCommandRead,\s*"
            r"vk::PipelineStageFlagBits2::eDrawIndirect\)"
        )
        self.assertGreaterEqual(len(exact_barrier.findall(RASTERIZER_CPP)), 2)

    def test_download_completion_has_o1_empty_gate(self):
        self.assertIn("bool HasPendingDownloadImages() const noexcept", TEXTURE_CACHE_H)
        self.assertIn("return !download_images.empty();", TEXTURE_CACHE_H)
        body = function_body(RASTERIZER_CPP, "void Rasterizer::ProcessDownloadImages()")
        self.assertIn("scheduler.HasPendingBufferTransfers()", body)
        self.assertIn("texture_cache.HasPendingDownloadImages()", body)
        self.assertIn("ExecutorResolvePhysicalDrs(\"download_images\")", body)
        self.assertIn(
            "if (!pending_transfers && !pending_downloads && !resolved_physical_drs)",
            body,
        )


if __name__ == "__main__":
    unittest.main()
