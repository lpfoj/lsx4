import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
VECTOR_ALU = (
    ROOT
    / "funnel-arm"
    / "src"
    / "shader_recompiler"
    / "frontend"
    / "translate"
    / "vector_alu.cpp"
)


class Ps4ShaderMbcntContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.source = VECTOR_ALU.read_text(encoding="utf-8")
        start = cls.source.index("void Translator::V_MBCNT_U32_B32")
        end = cls.source.index("void Translator::V_ADD_I32", start)
        cls.body = cls.source[start:end]

    def test_mbcnt_accepts_all_legal_source_operand_forms(self) -> None:
        self.assertNotIn("UNREACHABLE", self.body)
        self.assertIn("get_source(inst.src[0])", self.body)
        self.assertIn("get_source(inst.src[1])", self.body)
        self.assertIn("return GetSrc(operand)", self.body)
        self.assertIn("OperandField::ExecLo", self.body)
        self.assertIn("OperandField::ExecHi", self.body)
        self.assertIn("ir.Ballot(ir.GetExec())", self.body)
        self.assertIn("operand.field == OperandField::ExecHi ? 1 : 0", self.body)

    def test_mbcnt_models_both_halves_of_a_ps4_wave64(self) -> None:
        self.assertIn("ir.LaneId()", self.body)
        self.assertIn("ir.Imm32(31U)", self.body)
        self.assertIn("ir.Imm32(32U)", self.body)
        self.assertIn("ir.Imm32(~0U)", self.body)
        self.assertIn("is_low ? preceding_half_mask", self.body)
        self.assertIn("is_low ? ir.Imm32(~0U)", self.body)

    def test_compute_mbcnt_derives_ps4_wave_lane_without_host_subgroup_dependency(self) -> None:
        self.assertIn("runtime_info.stage != Stage::Compute", self.body)
        self.assertEqual(self.body.count("IR::Attribute::LocalInvocationId"), 3)
        self.assertIn("workgroup_size[0]", self.body)
        self.assertIn("workgroup_size[1]", self.body)
        self.assertIn("linear_local_invocation", self.body)
        self.assertIn("ir.Imm32(63U)", self.body)

    def test_mbcnt_counts_masked_bits_and_accumulates_src1(self) -> None:
        self.assertIn("ir.BitwiseAnd(src0, lane_mask)", self.body)
        self.assertIn("ir.BitCount", self.body)
        self.assertIn("ir.IAdd", self.body)
        self.assertIn("src1", self.body)


if __name__ == "__main__":
    unittest.main()
