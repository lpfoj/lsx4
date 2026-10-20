import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
ABI = (ROOT / "src/executor/fex_backend_abi_v3.h").read_text(encoding="utf-8")
LOADER = (ROOT / "src/executor/fex_backend_loader.cpp").read_text(encoding="utf-8")
RUNTIME = (ROOT / "src/executor/native_runtime_api.cpp").read_text(encoding="utf-8")


class FexPersistentRipSeedContractTests(unittest.TestCase):
    def test_candidate_is_explicit_and_default_off(self):
        self.assertIn("LSX4_FEX_CONFIG_PERSISTENT_RIP_SEED_AB", ABI)
        self.assertIn('root / "run-enable-fex-persistent-rip-seed"', RUNTIME)
        self.assertIn("defaultOff=1", RUNTIME)

    def test_namespace_isolated_by_candidate_and_bridge_module_identity(self):
        self.assertIn('"-ab-ripseed1"', LOADER)
        self.assertNotIn("persistent_seed_title_suffix", RUNTIME)

    def test_only_original_immutable_executable_segments_are_marked(self):
        ranges = RUNTIME.split("bool BuildExecutorFexMappedEntryRequest", 1)[1]
        marker = ranges.index("LSX4_FEX_RANGE_PERSISTENT_MODULE")
        guard = ranges.rfind("!is_funnel_slab && !is_imt_slab", 0, marker)
        enabled = ranges.rfind("g_fex_persistent_rip_seed_enabled.load", 0, marker)
        writable = ranges.rfind("LSX4_FEX_RANGE_WRITABLE", 0, marker)
        self.assertGreater(guard, 0)
        self.assertGreater(enabled, 0)
        self.assertLess(enabled, guard)
        self.assertGreater(writable, guard)


if __name__ == "__main__":
    unittest.main()
