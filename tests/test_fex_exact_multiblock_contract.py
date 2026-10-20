import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
ABI = (ROOT / "src/executor/fex_backend_abi_v3.h").read_text(encoding="utf-8")
LOADER = (ROOT / "src/executor/fex_backend_loader.cpp").read_text(encoding="utf-8")
RUNTIME = (ROOT / "src/executor/native_runtime_api.cpp").read_text(encoding="utf-8")


class FexExactMultiblockContractTests(unittest.TestCase):
    def test_append_only_configure_bit(self):
        self.assertIn(
            "LSX4_FEX_CONFIG_EXACT_MULTIBLOCK_AB = UINT32_C(1) << 2", ABI
        )
        self.assertIn(
            "LSX4_FEX_CONFIG_MAXINST_512_AB = UINT32_C(1) << 3", ABI
        )
        self.assertIn(
            "LSX4_FEX_CONFIG_MAXINST_256_AB = UINT32_C(1) << 4", ABI
        )

    def test_cpu_profile_is_passed_without_relaxing_memory(self):
        self.assertIn("cpu_profile.block_linking != 0", RUNTIME)
        self.assertIn("preflight.configure_flags |=", RUNTIME)
        self.assertIn("LSX4_FEX_CONFIG_EXACT_MULTIBLOCK_AB", RUNTIME)
        self.assertIn("metadata=immutable-v1 source=cpu-profile", RUNTIME)
        self.assertIn("BlockSizeMode::Mega", RUNTIME)
        self.assertIn("BlockSizeMode::Safe", RUNTIME)
        self.assertIn("exact_multiblock_requested && maxinst_512_requested", RUNTIME)
        self.assertIn("exact_multiblock_requested && maxinst_256_requested", RUNTIME)

    def test_loader_accepts_flag_and_isolates_native_cache(self):
        self.assertIn("kKnownConfigureFlags", LOADER)
        self.assertIn("LSX4_FEX_CONFIG_EXACT_MULTIBLOCK_AB", LOADER)
        self.assertIn('"-ab-mb1"', LOADER)
        self.assertIn('"-ab-mi512"', LOADER)
        self.assertIn('"-ab-mi256"', LOADER)
        self.assertIn('"conflicting-maxinst-geometry-flags"', LOADER)
        relaxed = LOADER.split("const bool relaxed_memory_ab", 1)[1].split(
            "if ((request.configure_flags", 1
        )[0]
        self.assertNotIn("EXACT_MULTIBLOCK", relaxed)


if __name__ == "__main__":
    unittest.main()
