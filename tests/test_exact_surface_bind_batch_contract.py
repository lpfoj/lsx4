import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
FUNNEL = ROOT / "funnel-arm" / "src" / "video_core"


class ExactSurfaceBindBatchContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.gate = (FUNNEL / "vulkan_driver_optimization.cpp").read_text(
            encoding="utf-8"
        )
        cls.cache_h = (FUNNEL / "texture_cache" / "texture_cache.h").read_text(
            encoding="utf-8"
        )
        cls.cache = (FUNNEL / "texture_cache" / "texture_cache.cpp").read_text(
            encoding="utf-8"
        )
        cls.rasterizer = (
            FUNNEL / "renderer_vulkan" / "vk_rasterizer.cpp"
        ).read_text(encoding="utf-8")

    def test_candidate_is_default_off_and_managed_only(self):
        self.assertIn("run-enable-exact-surface-bind-batch", self.gate)
        self.assertIn(
            "return IsManagedVulkanDriverOptimization() && allowed;", self.gate
        )

    def test_batch_owns_one_texture_cache_lock(self):
        self.assertIn("class ExactSurfaceBindBatch", self.cache_h)
        self.assertIn("std::unique_lock<std::mutex> lock", self.cache_h)
        self.assertIn("lock{cache.mutex}", self.cache)

    def test_batch_accepts_only_the_existing_full_exact_proof(self):
        helper = self.cache.split(
            "ImageId TextureCache::FindExactSurfaceHotImageLocked", 1
        )[1].split("ImageId TextureCache::FindImageExactHot", 1)[0]
        for proof in (
            "hot_image.image_uid == hot.image_uid",
            "ImageFlagBits::Registered",
            "hot_info.guest_address == info.guest_address",
            "hot_info.guest_size == info.guest_size",
            "hot_info.size == info.size",
            "hot_info.pitch == info.pitch",
            "hot_info.resources == info.resources",
            "hot_info.num_samples == info.num_samples",
            "hot_info.num_bits == info.num_bits",
            "hot_info.pixel_format == info.pixel_format",
            "hot_info.type == info.type",
            "hot_info.tile_mode == info.tile_mode",
        ):
            self.assertIn(proof, helper)

    def test_full_lookup_reuses_same_exact_helper(self):
        find_image = self.cache.split("ImageId TextureCache::FindImage(", 1)[1]
        self.assertIn("FindExactSurfaceHotImageLocked(info)", find_image)

    def test_admission_rejects_every_unsafe_context(self):
        admission = self.rasterizer.split(
            "const bool exact_surface_bind_batch_allowed", 1
        )[1].split("if (exact_surface_bind_batch_allowed)", 1)[0]
        for gate in (
            "!scheduler.HasPendingBufferTransfers()",
            "!executor_physical_drs_active",
            "!exec_live_resource_diag",
            "!texture_cache.ExecutorGpuContentProbeEnabled()",
            "!ExecutorAndroidRendererProbesEnabled()",
            "!Libraries::GnmDriver::ExecutorReplayActive()",
            "!Libraries::GnmDriver::ExecutorGnmCaptureActive()",
        ):
            self.assertIn(gate, admission)

    def test_miss_and_view_pass_keep_original_transactions(self):
        bind_textures = self.rasterizer.split("void Rasterizer::BindTextures", 1)[1]
        miss = bind_textures.split("if (!image_id)", 1)[1].split("#else", 1)[0]
        self.assertLess(miss.index("exact_surface_bind_batch->End()"),
                        miss.index("texture_cache.FindImage(desc)"))
        view_pass = bind_textures.split(
            "for (auto& [image_id, desc] : image_bindings)", 1
        )[0]
        self.assertIn("exact_surface_bind_batch->End()", view_pass[-1200:])
        self.assertIn("texture_cache.FindTexture(image_id, desc)", bind_textures)


if __name__ == "__main__":
    unittest.main()
