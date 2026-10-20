import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = (
    ROOT
    / "funnel-arm"
    / "src"
    / "video_core"
    / "texture_cache"
    / "texture_cache.cpp"
).read_text(encoding="utf-8")


class GpuImagePrewriteBufferPublicationContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.body = SOURCE.split(
            "void TextureCache::PublishGpuImagesBeforeCpuWrite", 1
        )[1].split("void TextureCache::InvalidateMemoryLocked", 1)[0]

    def test_successful_partial_image_readback_publishes_the_complete_backing(self):
        self.assertIn(
            "published_ranges.emplace_back(image_begin, image_size)", self.body
        )
        self.assertIn(
            "buffer_cache.InvalidateMemory(published_addr, published_size)",
            self.body,
        )

    def test_buffer_publication_happens_after_texture_cache_unlock(self):
        texture_scope_end = self.body.index(
            "// TryWriteBacking bypasses guest write faults"
        )
        buffer_publication = self.body.index("buffer_cache.InvalidateMemory")
        self.assertLess(texture_scope_end, buffer_publication)

    def test_full_overwrite_keeps_the_no_readback_fast_path(self):
        self.assertIn("if (fully_overwritten || !image.SafeToDownload())", self.body)


if __name__ == "__main__":
    unittest.main()
