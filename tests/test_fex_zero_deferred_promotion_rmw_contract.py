import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
ABI = (ROOT / "src/executor/fex_backend_abi_v3.h").read_text(encoding="utf-8")
LOADER = (ROOT / "src/executor/fex_backend_loader.cpp").read_text(encoding="utf-8")
RUNTIME = (ROOT / "src/executor/native_runtime_api.cpp").read_text(encoding="utf-8")


class FexZeroDeferredPromotionRmwContractTests(unittest.TestCase):
    def test_preserve_bit_is_append_only_and_known(self):
        self.assertIn(
            "LSX4_FEX_CONFIG_PRESERVE_ZERO_DEFERRED_PROMOTION_RMW_AB = "
            "UINT32_C(1) << 8",
            ABI,
        )
        known = LOADER.split("constexpr std::uint32_t kKnownConfigureFlags", 1)[1].split(
            ";", 1
        )[0]
        self.assertIn(
            "LSX4_FEX_CONFIG_PRESERVE_ZERO_DEFERRED_PROMOTION_RMW_AB", known
        )

    def test_default_skips_only_zero_counter_update(self):
        route = RUNTIME.split("preserve_zero_deferred_promotion_rmw_marker", 1)[1].split(
            "const auto preserve_hle_dispatch_exclusive_lock_marker", 1
        )[0]
        self.assertIn("run-preserve-fex-zero-deferred-promotion-rmw", RUNTIME)
        self.assertIn("std::filesystem::is_regular_file", route)
        self.assertIn(
            "LSX4_FEX_CONFIG_PRESERVE_ZERO_DEFERRED_PROMOTION_RMW_AB", route
        )
        self.assertIn("preserve=%d skipZero=%d", route)
        self.assertIn("nonzeroCounterExact=1", route)


if __name__ == "__main__":
    unittest.main()
