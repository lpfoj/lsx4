import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
IMGUI_VULKAN = (
    ROOT / "funnel-arm/src/imgui/renderer/imgui_impl_vulkan.cpp"
).read_text(encoding="utf-8")


class ImGuiTurnipPersistentMapAbContractTests(unittest.TestCase):
    def test_ab_requires_exact_marker_and_turnip_driver_identity(self):
        self.assertIn("run-ab-turnip-imgui-persistent-map", IMGUI_VULKAN)
        self.assertIn("vk::PhysicalDeviceDriverProperties", IMGUI_VULKAN)
        self.assertIn("driver_id == vk::DriverId::eMesaTurnip", IMGUI_VULKAN)
        self.assertIn("[EXECUTOR_IMGUI_PERSISTENT_MAP_AB]", IMGUI_VULKAN)

    def test_default_path_still_maps_and_unmaps_each_frame(self):
        render_start = IMGUI_VULKAN.index("void RenderDrawData(")
        render_end = IMGUI_VULKAN.index("static void DestroyFontsTexture();", render_start)
        render_body = IMGUI_VULKAN[render_start:render_end]
        self.assertIn("transient_vertex_map", render_body)
        self.assertIn("transient_index_map", render_body)
        self.assertIn("v.device.mapMemory", render_body)
        self.assertIn("v.device.unmapMemory(frb.vertex.buffer_memory);", render_body)
        self.assertIn("v.device.unmapMemory(frb.index.buffer_memory);", render_body)

    def test_noncoherent_flush_is_preserved_for_persistent_maps(self):
        render_start = IMGUI_VULKAN.index("void RenderDrawData(")
        render_end = IMGUI_VULKAN.index("static void DestroyFontsTexture();", render_start)
        render_body = IMGUI_VULKAN[render_start:render_end]
        self.assertIn("v.device.flushMappedMemoryRanges({range})", render_body)
        self.assertGreaterEqual(render_body.count(".size = VK_WHOLE_SIZE"), 2)

    def test_persistent_maps_are_removed_before_resize_and_free(self):
        resize_start = IMGUI_VULKAN.index("static void CreateOrResizeBuffer(")
        resize_end = IMGUI_VULKAN.index("static void SetupRenderState(", resize_start)
        resize_body = IMGUI_VULKAN[resize_start:resize_end]
        self.assertLess(resize_body.index("unmapMemory"), resize_body.index("destroyBuffer"))
        self.assertLess(resize_body.index("unmapMemory"), resize_body.index("freeMemory"))

        destroy_start = IMGUI_VULKAN.index("static void DestroyFrameRenderBuffers(")
        destroy_end = IMGUI_VULKAN.index("static void DestroyWindowRenderBuffers(", destroy_start)
        destroy_body = IMGUI_VULKAN[destroy_start:destroy_end]
        self.assertLess(destroy_body.index("unmapMemory"), destroy_body.index("destroyBuffer"))
        self.assertLess(destroy_body.index("unmapMemory"), destroy_body.index("freeMemory"))


if __name__ == "__main__":
    unittest.main()
