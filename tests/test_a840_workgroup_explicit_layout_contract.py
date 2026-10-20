import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
VK = ROOT / "funnel-arm" / "src" / "video_core" / "renderer_vulkan"
SHADER = ROOT / "funnel-arm" / "src" / "shader_recompiler"


class A840Non32BitSharedContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.capability = (VK / "vk_capability_profile.h").read_text(encoding="utf-8")
        cls.cache = (VK / "vk_pipeline_cache.cpp").read_text(encoding="utf-8")
        cls.docs = (ROOT / "docs" / "ANDROID_VULKAN_CAPABILITY_BRIDGE.md").read_text(
            encoding="utf-8"
        )
        cls.serialization = (VK / "vk_pipeline_serialization.cpp").read_text(
            encoding="utf-8"
        )
        cls.profile = (SHADER / "profile.h").read_text(encoding="utf-8")
        cls.simplify = (
            SHADER / "ir" / "passes" / "shared_memory_simplify_pass.cpp"
        ).read_text(encoding="utf-8")
        cls.shared_storage = (
            SHADER / "ir" / "passes" / "shared_memory_to_storage_pass.cpp"
        ).read_text(encoding="utf-8")
        cls.recompiler = (SHADER / "recompiler.cpp").read_text(encoding="utf-8")
        cls.emit = (SHADER / "backend" / "spirv" / "emit_spirv.cpp").read_text(
            encoding="utf-8"
        )
        cls.emit_context = (
            SHADER / "backend" / "spirv" / "spirv_emit_context.cpp"
        ).read_text(encoding="utf-8")

    def test_quirk_is_exact_to_proprietary_a840(self):
        quirk = "LowerNon32BitSharedToU32"
        route = self.capability.split(
            "constexpr u64 ExactProprietaryAdreno840PipelineQuirks", 1
        )[1].split("constexpr u64 ExactProprietaryAdreno750Quirks", 1)[0]
        self.assertIn(quirk, route)
        self.assertIn("IsExactProprietaryAdreno840", route)

        guards = self.capability.split(
            "static_assert(IsExactProprietaryAdreno840", 1
        )[1]
        for excluded in (
            "MobileDriverFamily::MesaTurnip",
            "MobileDriverFamily::ArmProprietary",
            "Adreno750DeviceId",
        ):
            self.assertIn(excluded, guards)

    def test_a840_keeps_advertised_explicit_layout_capability(self):
        assignment = self.cache.split(
            ".supports_workgroup_explicit_memory_layout =", 1
        )[1].split(".supports_amd_shader_explicit_vertex_parameter", 1)[0]
        self.assertIn("IsWorkgroupMemoryExplicitLayoutSupported()", assignment)
        self.assertNotIn("HasQuirk", assignment)
        self.assertNotIn("&&", assignment)
        self.assertIn("profile.supports_workgroup_explicit_memory_layout", self.emit)

    def test_u64_is_lowered_before_storage_and_info_collection(self):
        simplify = self.recompiler.index("SharedMemorySimplifyPass")
        to_storage = self.recompiler.index("SharedMemoryToStoragePass")
        collect_info = self.recompiler.index("CollectShaderInfoPass")
        self.assertLess(simplify, to_storage)
        self.assertLess(to_storage, collect_info)

        self.assertIn(
            "HasCodegenQuirk(CodegenQuirk::LowerNon32BitSharedToU32)",
            self.simplify,
        )
        self.assertIn("case IR::Opcode::LoadSharedU16", self.simplify)
        self.assertIn("case IR::Opcode::WriteSharedU16", self.simplify)
        self.assertIn("False(atomic_types & IR::Type::U64)", self.simplify)
        self.assertIn("case IR::Opcode::LoadSharedU64", self.simplify)
        self.assertIn("case IR::Opcode::WriteSharedU64", self.simplify)
        u64_load = self.simplify.rsplit("case IR::Opcode::LoadSharedU64", 1)[1].split(
            "case IR::Opcode::WriteSharedU64", 1
        )[0]
        u64_write = self.simplify.rsplit("case IR::Opcode::WriteSharedU64", 1)[
            1
        ].split("default:", 1)[0]
        self.assertEqual(u64_load.count("ir.LoadShared(32"), 2)
        self.assertEqual(u64_write.count("ir.WriteShared(32"), 2)

    def test_single_u32_view_returns_before_ssbo_fallback(self):
        early_return = self.shared_storage.split(
            "const auto used_types = CalculateSharedMemoryTypes(program);", 1
        )[1].split("const u32 binding", 1)[0]
        self.assertIn(
            "profile.supports_workgroup_explicit_memory_layout", early_return
        )
        self.assertIn(
            "std::popcount(static_cast<u32>(used_types)) == 1", early_return
        )
        self.assertIn("return;", early_return)

        fallback = self.shared_storage.split("const u32 binding", 1)[1]
        self.assertIn("BufferType::SharedMemory", fallback)
        self.assertNotIn("BufferType::SharedMemory", early_return)

    def test_codegen_quirk_has_distinct_shader_cache_identity(self):
        self.assertIn("LowerNon32BitSharedToU32 = 1U << 3", self.profile)
        self.assertGreaterEqual(self.serialization.count("profile.codegen_quirks"), 2)

    def test_broad_capability_disable_cannot_return(self):
        obsolete = "DisableWorkgroupExplicitMemoryLayoutCodegen"
        self.assertNotIn(obsolete, self.capability)
        self.assertNotIn(obsolete, self.cache)
        self.assertNotIn(obsolete, self.docs)

    def test_typed_buffer_views_keep_one_unified_binding(self):
        define_buffer = self.emit_context.split("EmitContext::BufferSpv", 1)[1].split(
            "void EmitContext::DefineBuffers", 1
        )[0]
        define_buffers = self.emit_context.split("void EmitContext::DefineBuffers", 1)[
            1
        ].split("spv::ImageFormat GetFormat", 1)[0]
        self.assertIn("spv::Decoration::Binding, binding.unified", define_buffer)
        self.assertEqual(define_buffers.count("++binding.unified;"), 1)

    def test_route_has_no_title_or_shader_hash_hardcode(self):
        route = self.capability.split(
            "constexpr u64 ExactProprietaryAdreno840PipelineQuirks", 1
        )[1].split("constexpr u64 ExactProprietaryAdreno750Quirks", 1)[0]
        for forbidden in ("CUSA", "615f180c", "Alienation"):
            self.assertNotIn(forbidden, route)


if __name__ == "__main__":
    unittest.main()
