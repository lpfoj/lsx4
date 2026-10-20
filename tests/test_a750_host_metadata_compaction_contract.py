from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
VIDEO = ROOT / "funnel-arm" / "src" / "video_core"
PAGE_TABLE = (VIDEO / "multi_level_page_table.h").read_text(encoding="utf-8")
PROFILE = (VIDEO / "renderer_vulkan" / "vk_capability_profile.h").read_text(
    encoding="utf-8"
)
INSTANCE = (VIDEO / "renderer_vulkan" / "vk_instance.cpp").read_text(
    encoding="utf-8"
)
TEXTURE = (VIDEO / "texture_cache" / "texture_cache.cpp").read_text(
    encoding="utf-8"
)
BUFFER = (VIDEO / "buffer_cache" / "buffer_cache.cpp").read_text(
    encoding="utf-8"
)
IMGUI_H = (ROOT / "funnel-arm" / "src" / "imgui" / "renderer" / "imgui_impl_vulkan.h").read_text(
    encoding="utf-8"
)
IMGUI = (ROOT / "funnel-arm" / "src" / "imgui" / "renderer" / "imgui_impl_vulkan.cpp").read_text(
    encoding="utf-8"
)


class A750HostMetadataCompactionContract(unittest.TestCase):
    def test_page_pool_chunk_is_bounded_by_addressable_l1_pages(self):
        self.assertIn("size_t page_pool_chunk_size = 8192", PAGE_TABLE)
        self.assertIn("page_pool_chunk_size == 0", PAGE_TABLE)
        self.assertIn("? 1", PAGE_TABLE)
        self.assertIn("1ULL << FirstLevelBits", PAGE_TABLE)
        self.assertIn("page_pool_chunk_size > (1ULL << FirstLevelBits)", PAGE_TABLE)

    def test_small_chunks_are_exact_proprietary_a750_only(self):
        self.assertIn("optimization_a750_host_metadata_compaction", PROFILE)
        self.assertIn("run-disable-sm8650-a750-host-metadata-compaction", INSTANCE)
        self.assertIn("capability_profile.exact_proprietary_adreno750 &&", INSTANCE)
        self.assertIn("? 16u", TEXTURE)
        self.assertIn(": 8192u", TEXTURE)
        self.assertIn("? 256u", BUFFER)
        self.assertIn(": 8192u", BUFFER)

    def test_font_cpu_pixels_are_released_after_upload_completion(self):
        self.assertIn("bool release_font_cpu_pixels{false};", IMGUI_H)
        queue_idle = IMGUI.index("queue.waitIdle")
        clear_pixels = IMGUI.index("io.Fonts->ClearTexData()")
        self.assertLess(queue_idle, clear_pixels)
        self.assertIn("release_font_cpu_pixels", IMGUI)


if __name__ == "__main__":
    unittest.main()
