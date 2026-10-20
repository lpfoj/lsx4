from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
FUNNEL = ROOT / "funnel-arm" / "src"


class A750SharedMemoryViewContract(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.profile = (FUNNEL / "shader_recompiler" / "profile.h").read_text()
        cls.pass_cpp = (
            FUNNEL
            / "shader_recompiler"
            / "ir"
            / "passes"
            / "shared_memory_simplify_pass.cpp"
        ).read_text()
        cls.capability = (
            FUNNEL
            / "video_core"
            / "renderer_vulkan"
            / "vk_capability_profile.h"
        ).read_text()
        cls.pipeline_cache = (
            FUNNEL
            / "video_core"
            / "renderer_vulkan"
            / "vk_pipeline_cache.cpp"
        ).read_text()

    def test_exact_proprietary_a750_route_only(self):
        self.assertIn("LowerAliasedSharedU64ToU32 = 1ULL << 17", self.capability)
        exact = self.capability.split(
            "constexpr u64 ExactProprietaryAdreno750PipelineQuirks", 1
        )[1].split("constexpr u64 ExactProprietaryAdreno750Quirks", 1)[0]
        self.assertIn("IsExactProprietaryAdreno750", exact)
        self.assertIn("CapabilityQuirk::LowerAliasedSharedU64ToU32", exact)
        self.assertNotIn("title", exact.lower())
        self.assertNotIn("CUSA", exact)

    def test_profile_bit_is_serialized_in_existing_identity_byte(self):
        self.assertIn("LowerAliasedSharedU64ToU32 = 1U << 2", self.profile)
        self.assertIn("static_assert(sizeof(Profile) == 64)", self.profile)
        self.assertIn(
            "CapabilityQuirk::LowerAliasedSharedU64ToU32", self.pipeline_cache
        )
        self.assertIn(
            "Shader::CodegenQuirk::LowerAliasedSharedU64ToU32", self.pipeline_cache
        )

    def test_only_mixed_u32_non_atomic_u64_is_lowered(self):
        self.assertIn("ProgramRequires32BitSharedView(program)", self.pass_cpp)
        self.assertIn("False(atomic_types & IR::Type::U64)", self.pass_cpp)
        self.assertIn("case IR::Opcode::WriteSharedU64", self.pass_cpp)
        self.assertIn("case IR::Opcode::LoadSharedU64", self.pass_cpp)
        self.assertIn("ir.UnpackUint2x32", self.pass_cpp)
        self.assertIn("ir.PackUint2x32", self.pass_cpp)
        self.assertNotIn("std::mutex", self.pass_cpp)
        self.assertNotIn("WaitIdle", self.pass_cpp)

    def test_u16_fallback_stays_bound_to_missing_extension(self):
        self.assertIn(
            "lower_for_missing_explicit_layout && False(atomic_types & IR::Type::U16)",
            self.pass_cpp,
        )


if __name__ == "__main__":
    unittest.main()
