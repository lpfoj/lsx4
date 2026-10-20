from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
SAMPLER = (
    ROOT
    / "funnel-arm"
    / "src"
    / "video_core"
    / "texture_cache"
    / "sampler.cpp"
).read_text(encoding="utf-8")


class SamplerMipFilterNoneContractTest(unittest.TestCase):
    def test_none_clamps_view_lod_without_rewriting_shader_sampling(self):
        self.assertIn(
            "sampler.mip_filter.Value() == AmdGpu::MipFilter::None", SAMPLER
        )
        self.assertIn("min_filter == mag_filter", SAMPLER)
        self.assertIn("!AmdGpu::IsAnisoFilter(min_filter)", SAMPLER)
        self.assertIn("!AmdGpu::IsAnisoFilter(mag_filter)", SAMPLER)
        self.assertIn("force_base_level ? 0.0f : sampler.MinLod()", SAMPLER)
        self.assertIn("force_base_level ? 0.0f : sampler.MaxLod()", SAMPLER)
        self.assertIn(".minLod = effective_min_lod", SAMPLER)
        self.assertIn(".maxLod = effective_max_lod", SAMPLER)

    def test_fix_is_capability_driven_and_has_no_title_or_driver_gate(self):
        for forbidden in (
            "CUSA",
            "GameSerial",
            "Adreno",
            "Qualcomm",
            "Turnip",
            "Mali",
            "getenv",
        ):
            self.assertNotIn(forbidden, SAMPLER)


if __name__ == "__main__":
    unittest.main()
