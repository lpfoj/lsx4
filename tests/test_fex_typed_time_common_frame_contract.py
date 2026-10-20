import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
ABI = (ROOT / "src/executor/fex_backend_abi_v3.h").read_text(encoding="utf-8")
LOADER = (ROOT / "src/executor/fex_backend_loader.cpp").read_text(encoding="utf-8")
RUNTIME = (ROOT / "src/executor/native_runtime_api.cpp").read_text(encoding="utf-8")


class FexTypedTimeCommonFrameContractTests(unittest.TestCase):
    def test_control_bit_is_append_only_and_known(self):
        self.assertIn(
            "LSX4_FEX_CONFIG_PRESERVE_TYPED_TIME_COMMON_FRAME_AB = "
            "UINT32_C(1) << 7",
            ABI,
        )
        known = LOADER.split("constexpr std::uint32_t kKnownConfigureFlags", 1)[1].split(
            ";", 1
        )[0]
        self.assertIn("LSX4_FEX_CONFIG_PRESERVE_TYPED_TIME_COMMON_FRAME_AB", known)

    def test_marker_is_default_fast_and_process_immutable(self):
        route = RUNTIME.split("preserve_typed_time_common_frame_marker", 1)[1].split(
            "const bool maxinst_512_requested", 1
        )[0]
        self.assertIn('run-preserve-fex-typed-time-common-frame', RUNTIME)
        self.assertIn("std::filesystem::is_regular_file", route)
        self.assertIn("LSX4_FEX_CONFIG_PRESERVE_TYPED_TIME_COMMON_FRAME_AB", route)
        self.assertIn("preserve=%d outline=%d", route)


if __name__ == "__main__":
    unittest.main()
