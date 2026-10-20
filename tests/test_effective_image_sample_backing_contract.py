from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
IMAGE_CPP = (
    ROOT / "funnel-arm" / "src" / "video_core" / "texture_cache" / "image.cpp"
).read_text(encoding="utf-8")
IMAGE_H = (
    ROOT / "funnel-arm" / "src" / "video_core" / "texture_cache" / "image.h"
).read_text(encoding="utf-8")


class EffectiveImageSampleBackingContract(unittest.TestCase):
    def test_backing_identity_uses_supported_host_sample_count(self):
        body = IMAGE_CPP.split("void Image::SetBackingSamples", 1)[1]
        self.assertIn(
            "LiverpoolToVK::NumSamples(num_samples, supported_samples)", body
        )
        self.assertIn("backing->num_samples == host_num_samples", body)
        self.assertIn(
            "std::ranges::find(backing_images, host_num_samples", body
        )
        self.assertIn("new_backing->num_samples = host_num_samples", body)

    def test_copy_pipeline_uses_the_effective_sample_count(self):
        body = IMAGE_CPP.split("void Image::SetBackingSamples", 1)[1]
        self.assertIn(
            "info.size.width, info.size.height, new_backing->num_samples", body
        )
        self.assertNotIn(
            "new_backing->num_samples = num_samples", body
        )

    def test_initial_backing_uses_the_created_vulkan_sample_count(self):
        constructor = IMAGE_CPP.split("Image::Image(", 1)[1].split(
            "Image::~Image", 1
        )[0]
        self.assertIn(
            "backing->num_samples = static_cast<u32>(image_ci.samples)", constructor
        )

    def test_vma_reports_physical_allocation_bytes_at_creation(self):
        create = IMAGE_CPP.split("vk::Result UniqueImage::TryCreate", 1)[1].split(
            "Image::Image(", 1
        )[0]
        self.assertIn("VmaAllocationInfo allocation_info{}", create)
        self.assertIn("&unsafe_allocation, &allocation_info", create)
        self.assertIn("allocation_size = allocation_info.size", create)
        self.assertIn("u64 allocation_size{}", IMAGE_H)


if __name__ == "__main__":
    unittest.main()
