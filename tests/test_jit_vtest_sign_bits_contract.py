import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
POLICY = (
    ROOT / "src/executor/dynamic_translation/vector_lowering_policy.cpp"
).read_text(encoding="utf-8")
CORE = (
    ROOT / "src/executor/dynamic_translation/retiring_execution_core.cpp"
).read_text(encoding="utf-8")
LANES = (
    ROOT / "src/executor/dynamic_translation/packed_lane_ops.cpp"
).read_text(encoding="utf-8")
FOUNDATION = (
    ROOT / "src/executor/dynamic_translation/foundation_checks.cpp"
).read_text(encoding="utf-8")


class JitVtestSignBitsContractTests(unittest.TestCase):
    def test_vtestps_and_vtestpd_have_a_dedicated_direct_kernel(self):
        self.assertIn("case X86_MNEMONIC_VTESTPS:", POLICY)
        self.assertIn("case X86_MNEMONIC_VTESTPD:", POLICY)
        self.assertIn("DirectVectorKernel::PackedSignBitTest", POLICY)
        self.assertIn("width_bits / 8, 4", POLICY)
        self.assertIn("width_bits / 8, 8", POLICY)

    def test_semantics_only_consider_each_float_lane_sign_bit(self):
        body = LANES.split("PackedBitTestResult TestPackedSignBits", 1)[1]
        body = body.split("bool SumAbsoluteByteGroups", 1)[0]
        self.assertIn("CollectSignMask(left, lane_width, total_width)", body)
        self.assertIn("CollectSignMask(right, lane_width, total_width)", body)
        self.assertIn("(left_signs & right_signs) == 0", body)
        self.assertIn("((~left_signs) & right_signs) == 0", body)

    def test_arm64_codegen_stays_on_the_native_simd_fast_path(self):
        self.assertIn("DirectVectorKernel::PackedSignBitTest", CORE)
        self.assertIn("code->ushr(code->v4.s4, code->v4.s4, 31)", CORE)
        self.assertIn("code->ushr(code->v4.d2, code->v4.d2, 63)", CORE)
        self.assertIn("emitted_simd_fast += emitted ? 1u : 0u", CORE)

    def test_exact_islets_instruction_is_a_foundation_fixture(self):
        self.assertIn("0xc4, 0xe2, 0x79, 0x0e, 0xd2", FOUNDATION)
        self.assertIn("frame.condition_word & CarryFlag", FOUNDATION)
        self.assertIn("frame.condition_word & ZeroFlag", FOUNDATION)


if __name__ == "__main__":
    unittest.main()
