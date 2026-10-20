import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PS4_RUNTIME = (ROOT / "src/executor/native_runtime_api.cpp").read_text(
    encoding="utf-8"
)
PS5_RUNTIME = (ROOT / "src/executor/ps5_desktop/runtime_api.cpp").read_text(
    encoding="utf-8"
)


EXTENDED_ISA_MASK_PATTERN = re.compile(
    r"LSX4_FEX_HOST_ISA_FCMA\s*\|\s*"
    r"LSX4_FEX_HOST_ISA_FRINTTS\s*\|\s*"
    r"LSX4_FEX_HOST_ISA_ECV"
)


class FexExtendedIsaIsolationContractTests(unittest.TestCase):
    def test_ps5_preserves_baseline_codegen_on_every_host(self):
        preflight = PS5_RUNTIME.split(
            "Executor::Fex::PreflightRequest preflight{};", 1
        )[1].split("Executor::Fex::Preflight(preflight)", 1)[0]
        mask = re.search(
            r"preflight\.host_isa_feature_mask\s*&=\s*~\(\s*"
            + EXTENDED_ISA_MASK_PATTERN.pattern
            + r"\s*\);",
            preflight,
        )

        self.assertIsNotNone(mask)
        self.assertLess(mask.start(), preflight.index("effective_host_isa"))
        self.assertNotIn("IsSnapdragon8Gen3Host", preflight)
        self.assertNotIn("run-enable-sm8650-fex-extended-isa-ab", preflight)

    def test_ps4_extended_codegen_requires_exact_sm8650_opt_in(self):
        policy = PS4_RUNTIME.split(
            "constexpr std::uint64_t sm8650_extended_fex_isa =", 1
        )[1].split("const bool relax_vector_tso_ab", 1)[0]

        self.assertRegex(policy, EXTENDED_ISA_MASK_PATTERN)
        self.assertIn(
            'std::filesystem::is_regular_file(\n'
            '        root / "run-enable-sm8650-fex-extended-isa-ab")',
            policy,
        )
        self.assertIn(
            "Common::IsSnapdragon8Gen3Host() && "
            "sm8650_extended_fex_isa_requested",
            policy,
        )
        self.assertIn(
            "if (!sm8650_extended_fex_isa_enabled) {\n"
            "        preflight.host_isa_feature_mask &= "
            "~sm8650_extended_fex_isa;",
            policy,
        )


if __name__ == "__main__":
    unittest.main()
