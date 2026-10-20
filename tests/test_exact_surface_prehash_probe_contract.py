import pathlib
import re
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = ROOT / "funnel-arm" / "src" / "video_core" / "texture_cache" / "texture_cache.cpp"
HEADER = ROOT / "funnel-arm" / "src" / "video_core" / "texture_cache" / "texture_cache.h"


class ExactSurfacePrehashProbeContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.source = SOURCE.read_text(encoding="utf-8")
        cls.header = HEADER.read_text(encoding="utf-8")




        start = cls.source.index(
            "ImageId TextureCache::FindExactSurfaceHotImageLocked"
        )
        end = cls.source.index("struct ImageCandidate", start)
        cls.fast_path = cls.source[start:end]

    def test_candidate_keeps_markers_and_adds_only_exact_sm8650_d9_default(self):
        constructor_start = self.source.index("TextureCache::TextureCache")
        constructor_end = self.source.index("void TextureCache::RefreshManagedResidencyPolicy")
        constructor = self.source[constructor_start:constructor_end]
        self.assertIn('"run-enable-exact-surface-hot-cache"', constructor)
        self.assertIn('"run-enable-exact-surface-prehash-probe"', constructor)
        self.assertIn("Common::IsSnapdragon8Gen3Host()", constructor)
        self.assertIn(
            "instance.GetCapabilityProfile().exact_mesa_turnip_adreno750",
            constructor,
        )
        self.assertIn(
            "MesaTurnipA750Da14d65e44Ib32RolloverBoStats", constructor
        )
        self.assertIn(
            "exact_surface_hot_requested || exact_sm8650_d9_default",
            constructor,
        )
        self.assertIn(
            "exact_surface_prehash_requested || exact_sm8650_d9_default",
            constructor,
        )
        self.assertNotIn("Adreno840DeviceId", constructor)
        self.assertIn("bool exact_surface_prehash_probe_enabled = false;", self.header)

    def test_common_hit_precedes_exact_fingerprint(self):
        probe = self.fast_path.index("ExactSurfacePrehashProbeKey(info)")
        return_hit = self.fast_path.index("return hot.image_id", probe)
        exact_hash = self.fast_path.index("ExactSurfaceIdentity(info)", return_hit)
        self.assertLess(probe, return_hit)
        self.assertLess(return_hit, exact_hash)
        self.assertIn("fingerprintSkipped=%u", self.fast_path)

    def test_prehash_never_replaces_full_exact_proof(self):
        required = (
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
        )
        for proof in required:
            with self.subTest(proof=proof):
                self.assertIn(proof, self.fast_path)

    def test_miss_uses_original_exact_index_and_same_table_key_on_fill(self):
        self.assertIn("exact_surface_index.find(exact_identity)", self.fast_path)
        self.assertIn("hot_probe_key & (ExactSurfaceHotEntryCount - 1)", self.fast_path)
        self.assertIn("hot.identity = hot_probe_key;", self.fast_path)
        self.assertNotIn("exact_surface_index.find(hot_probe_key)", self.fast_path)


if __name__ == "__main__":
    unittest.main()
