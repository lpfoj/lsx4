import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
ADDRESS_SPACE = (ROOT / "funnel-arm/src/core/address_space.cpp").read_text(
    encoding="utf-8"
)


class AndroidPs4SparseVaContractTests(unittest.TestCase):
    def test_sparse_engine_heaps_are_canonical_and_owned(self):
        self.assertIn(
            "static constexpr u64 AndroidProbeUserSize = 8_GB;",
            ADDRESS_SPACE,
        )
        self.assertIn(
            "static constexpr VAddr AndroidProbeSecondaryUserMin = 0x2000000000ULL;",
            ADDRESS_SPACE,
        )
        self.assertIn(
            "static constexpr VAddr AndroidProbeTertiaryUserMin = 0x3000000000ULL;",
            ADDRESS_SPACE,
        )
        self.assertIn(
            "static constexpr VAddr AndroidProbeMidUserMin = 0x4000000000ULL;",
            ADDRESS_SPACE,
        )
        self.assertIn(
            "ReserveAndroidCanonicalRegion(\n            AndroidProbeSecondaryUserMin, android_secondary_user_size)",
            ADDRESS_SPACE,
        )
        self.assertIn(
            "ReserveAndroidCanonicalRegion(\n            AndroidProbeTertiaryUserMin, android_tertiary_user_size)",
            ADDRESS_SPACE,
        )
        self.assertIn(
            "ReserveAndroidCanonicalRegion(AndroidProbeMidUserMin, android_mid_user_size)",
            ADDRESS_SPACE,
        )
        self.assertIn("secondary_user_addr + android_secondary_user_size", ADDRESS_SPACE)
        self.assertIn("tertiary_user_addr + android_tertiary_user_size", ADDRESS_SPACE)
        self.assertIn(
            "m_free_regions.insert({mid_user_addr, mid_user_addr + android_mid_user_size})",
            ADDRESS_SPACE,
        )

    def test_optional_sparse_window_does_not_disable_core_ps4_layout(self):
        self.assertIn("if (android_secondary_user_base)", ADDRESS_SPACE)
        self.assertIn("android_secondary_user_size = 0;", ADDRESS_SPACE)
        self.assertIn("if (android_tertiary_user_base)", ADDRESS_SPACE)
        self.assertIn("android_tertiary_user_size = 0;", ADDRESS_SPACE)
        self.assertIn("if (android_mid_user_base)", ADDRESS_SPACE)
        self.assertIn("android_mid_user_size = 0;", ADDRESS_SPACE)
        self.assertNotIn("MAP_FIXED | MAP_ANONYMOUS | MAP_NORESERVE", ADDRESS_SPACE)


if __name__ == "__main__":
    unittest.main()
