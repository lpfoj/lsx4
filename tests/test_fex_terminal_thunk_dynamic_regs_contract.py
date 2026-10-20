import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
ABI = (ROOT / "src/executor/fex_backend_abi_v3.h").read_text(encoding="utf-8")
LOADER = (ROOT / "src/executor/fex_backend_loader.cpp").read_text(encoding="utf-8")
RUNTIME = (ROOT / "src/executor/native_runtime_api.cpp").read_text(encoding="utf-8")


class FexTerminalThunkDynamicRegsContractTests(unittest.TestCase):
    def test_same_binary_control_flag_is_append_only_and_known(self):
        self.assertIn(
            "LSX4_FEX_CONFIG_PRESERVE_TERMINAL_THUNK_DYNAMIC_REGS_AB = "
            "UINT32_C(1) << 6",
            ABI,
        )
        known = LOADER.split("constexpr std::uint32_t kKnownConfigureFlags", 1)[1]
        known = known.split("constexpr std::uint32_t kRequiredLifetimeFlags", 1)[0]
        self.assertIn(
            "LSX4_FEX_CONFIG_PRESERVE_TERMINAL_THUNK_DYNAMIC_REGS_AB", known
        )

    def test_default_preserves_registers_and_elision_is_explicit_opt_in(self):
        setup = RUNTIME.split(
            "run-enable-fex-terminal-thunk-dynamic-reg-elision", 1
        )
        self.assertEqual(2, len(setup))
        nearby = setup[1][:2600]
        self.assertIn(
            "LSX4_FEX_CONFIG_PRESERVE_TERMINAL_THUNK_DYNAMIC_REGS_AB", nearby
        )
        self.assertIn(
            "const bool preserve_terminal_thunk_dynamic_regs =\n"
            "        !terminal_thunk_dynamic_reg_elision_requested;",
            nearby,
        )
        self.assertIn("preserve=%d elide=%d", nearby)
        self.assertIn("scope=generic-safe-default", nearby)
        self.assertIn("abi-correctness-default", nearby)


if __name__ == "__main__":
    unittest.main()
