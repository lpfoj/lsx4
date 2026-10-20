from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
VK = ROOT / "funnel-arm" / "src" / "video_core" / "renderer_vulkan"
PIPELINE_CACHE = (VK / "vk_pipeline_cache.cpp").read_text(encoding="utf-8")
PIPELINE_SERIALIZATION = (VK / "vk_pipeline_serialization.cpp").read_text(
    encoding="utf-8"
)


class VulkanPipelineCacheIdentityEpochTest(unittest.TestCase):
    def test_native_driver_blob_has_own_compatibility_epoch(self):
        self.assertIn(
            "constexpr u32 HostPipelineCacheCompatibilityEpoch = 4u;",
            PIPELINE_CACHE,
        )
        self.assertIn('fmt::format("vk-native-v{}-', PIPELINE_CACHE)

    def test_native_driver_blob_is_scoped_to_the_exact_driver(self):
        host_name = PIPELINE_CACHE.split(
            "std::string PipelineCache::HostPipelineCacheName() const", 1
        )[1].split("bool PipelineCache::WriteCacheFileAtomically", 1)[0]
        for identity in (
            "instance.GetVendorID()",
            "instance.GetDeviceID()",
            "instance.GetDriverID()",
            "instance.GetDriverVersion()",
            "instance.ApiVersion()",
            "instance.GetPipelineCacheUUID()",
        ):
            self.assertIn(identity, host_name)

    def test_device_loss_never_persists_an_opaque_driver_blob(self):
        save = PIPELINE_CACHE.split(
            "void PipelineCache::SaveHostPipelineCache()", 1
        )[1].split("void PipelineCache::MarkHostPipelineCacheDirty()", 1)[0]
        self.assertIn("scheduler.IsDeviceLost()", save)
        self.assertLess(save.index("scheduler.IsDeviceLost()"),
                        save.index("getPipelineCacheData"))

    def test_pipeline_epoch_invalidates_warmup_without_discarding_spirv(self):
        self.assertIn(
            "constexpr u32 PipelineStorageNamespaceSchema = 3u;",
            PIPELINE_SERIALIZATION,
        )
        self.assertIn(
            "constexpr u32 ShaderStorageNamespaceSchema = 1u;",
            PIPELINE_SERIALIZATION,
        )
        self.assertIn(
            'fmt::format("vk-pipeline-v{}-', PIPELINE_SERIALIZATION
        )
        self.assertIn('fmt::format("vk-shader-v{}-', PIPELINE_SERIALIZATION)


if __name__ == "__main__":
    unittest.main()
