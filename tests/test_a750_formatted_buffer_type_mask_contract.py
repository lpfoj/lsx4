import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SHADER = ROOT / "funnel-arm" / "src" / "shader_recompiler"
VK = ROOT / "funnel-arm" / "src" / "video_core" / "renderer_vulkan"


class A750FormattedBufferTypeMaskContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.capability = (VK / "vk_capability_profile.h").read_text(encoding="utf-8")
        cls.cache = (VK / "vk_pipeline_cache.cpp").read_text(encoding="utf-8")
        cls.serialization = (VK / "vk_pipeline_serialization.cpp").read_text(
            encoding="utf-8"
        )
        cls.profile = (SHADER / "profile.h").read_text(encoding="utf-8")
        cls.recompiler = (SHADER / "recompiler.cpp").read_text(encoding="utf-8")
        cls.passes = (SHADER / "ir" / "passes" / "ir_passes.h").read_text(
            encoding="utf-8"
        )
        cls.tracking = (
            SHADER / "ir" / "passes" / "resource_tracking_pass.cpp"
        ).read_text(encoding="utf-8")
        cls.lower = (
            SHADER / "ir" / "passes" / "lower_buffer_format_to_raw.cpp"
        ).read_text(encoding="utf-8")
        cls.integer_emit = (
            SHADER / "backend" / "spirv" / "emit_spirv_integer.cpp"
        ).read_text(encoding="utf-8")

    def test_quirk_is_exact_proprietary_a750_only_and_wave64_ab_is_dormant(self):
        quirk = "DeferFormattedBufferTypesUntilLowering"
        self.assertIn(f"{quirk} = 1ULL << 16", self.capability)
        route = self.capability.split(
            "constexpr u64 ExactProprietaryAdreno750PipelineQuirks", 1
        )[1].split("constexpr u64 ExactProprietaryAdreno840PipelineQuirks", 1)[0]
        self.assertIn("IsExactProprietaryAdreno750", route)
        returned_quirks = route.split("return static_cast<u64>(", 1)[1]
        self.assertIn(quirk, returned_quirks)
        self.assertNotIn(
            "AvoidRequiredSubgroupSizeForSubgroupIndependentCompute",
            returned_quirks,
        )

        exact_match = self.capability.split(
            "constexpr bool IsExactProprietaryAdreno750", 1
        )[1].split("constexpr bool EnableExactProprietaryAdreno750BarrierCapture", 1)[0]
        self.assertIn("MobileDriverFamily::QualcommProprietary", exact_match)
        self.assertIn("vendor_id == QualcommVendorId", exact_match)
        self.assertIn("device_id == Adreno750DeviceId", exact_match)
        self.assertIn("constexpr u32 QualcommVendorId = 0x5143", self.capability)
        self.assertIn("constexpr u32 Adreno750DeviceId = 0x43051401", self.capability)
        for excluded_route in (
            "MobileDriverFamily::MesaTurnip, QualcommVendorId",
            "MobileDriverFamily::ArmProprietary, 0x13b5",
            "QualcommVendorId,\n                  0x44050a31",
        ):
            self.assertIn(excluded_route, self.capability)

    def test_codegen_mask_preserves_the_former_clamp_byte_contract(self):
        self.assertIn("BrokenSpirvClamp = 1U << 0", self.profile)
        self.assertIn("DeferFormattedBufferTypesUntilLowering = 1U << 1", self.profile)
        self.assertIn("u8 codegen_quirks{}", self.profile)
        self.assertNotIn("bool has_broken_spirv_clamp", self.profile)
        self.assertIn("static_assert(sizeof(CodegenQuirk) == sizeof(u8))", self.profile)
        self.assertIn("static_assert(sizeof(Profile) == 64)", self.profile)
        self.assertIn(
            "static_assert(static_cast<u8>(CodegenQuirk::BrokenSpirvClamp) == 1U)",
            self.profile,
        )
        self.assertGreaterEqual(
            self.integer_emit.count(
                "HasCodegenQuirk(CodegenQuirk::BrokenSpirvClamp)"
            ),
            2,
        )

    def test_capability_maps_to_codegen_and_changes_only_affected_shader_identity(self):
        profile_init = self.cache.split("profile = Shader::Profile{", 1)[1].split(
            "std::vector<u8> host_cache_data", 1
        )[0]
        self.assertIn(".codegen_quirks =", profile_init)
        self.assertIn("CapabilityQuirk::DeferFormattedBufferTypesUntilLowering", profile_init)
        self.assertIn("Shader::CodegenQuirk::DeferFormattedBufferTypesUntilLowering", profile_init)



        self.assertEqual(self.serialization.count("profile.codegen_quirks"), 2)
        self.assertGreaterEqual(
            self.serialization.count("const std::array<u8, 31> profile_flags"), 2
        )
        self.assertNotIn("profile.has_broken_spirv_clamp", self.serialization)
        self.assertIn("PipelineStorageDriverQuirks(capability.driver_quirks)", self.serialization)

    def test_resource_tracking_defers_only_formatted_compute_accesses(self):
        route = self.tracking.split("void ResourceTrackingPass", 1)[1]
        route = route.split("\n}\n\n}", 1)[0]
        self.assertIn("info.stage == Stage::Compute", route)
        self.assertIn(
            "profile.HasCodegenQuirk(CodegenQuirk::DeferFormattedBufferTypesUntilLowering)",
            route,
        )
        self.assertIn(
            "PatchBufferSharp(*block, inst, info, descriptors, defer_formatted_types)",
            route,
        )

        buffer_data_type = self.tracking.split("IR::Type BufferDataType", 1)[1].split(
            "u32 BufferAddressShift", 1
        )[0]
        type_case = buffer_data_type.split(
            "case IR::Opcode::LoadBufferFormatF32:", 1
        )[1].split("default:", 1)[0]
        self.assertIn("IR::Type::Void", type_case)
        self.assertIn(
            "IR::Type::U32 | IR::Type::F32 | IR::Type::U16 | IR::Type::U8",
            type_case,
        )

        self.assertEqual(
            self.tracking.count(
                "BufferDataType(inst, buffer.GetNumberFmt(), defer_formatted_types)"
            ),
            2,
        )

    def test_lowering_ors_only_the_selected_raw_type(self):
        raw_types = self.lower.split("static IR::Type RawBufferType", 1)[1].split(
            "static IR::Value LoadBufferFormat", 1
        )[0]
        for data_format, raw_type in {
            "FormatInvalid": "Void",
            "Format8": "U8",
            "Format8_8": "U16",
            "Format16": "U16",
            "Format8_8_8_8": "U32",
            "Format16_16": "U32",
            "Format10_11_11": "U32",
            "Format2_10_10_10": "U32",
            "Format16_16_16_16": "U32",
            "Format32": "F32",
            "Format32_32": "F32",
            "Format32_32_32": "F32",
            "Format32_32_32_32": "F32",
        }.items():
            self.assertIn(f"AmdGpu::DataFormat::{data_format}", raw_types)
            self.assertIn(f"IR::Type::{raw_type}", raw_types)
        self.assertIn("descriptor-layout slot", raw_types)
        self.assertIn("emit no SPIR-V buffer type", raw_types)

        lowering = self.lower.split("void LowerBufferFormatToRaw", 1)[1]
        self.assertIn("info.stage == Stage::Compute", lowering)
        self.assertIn(
            "profile.HasCodegenQuirk(CodegenQuirk::DeferFormattedBufferTypesUntilLowering)",
            lowering,
        )
        self.assertIn(
            "info.buffers[binding].used_types |= RawBufferType(data_format)",
            self.lower,
        )
        self.assertNotIn("info.buffers[binding].used_types = RawBufferType", self.lower)

    def test_pass_plumbing_adds_no_runtime_sync_or_title_special_case(self):
        self.assertIn("LowerBufferFormatToRaw(IR::Program& program, const Profile& profile)", self.passes)
        self.assertIn("LowerBufferFormatToRaw(program, profile)", self.recompiler)
        focused = self.tracking + self.lower
        for forbidden in (
            "CUSA",
            "std::mutex",
            "scoped_lock",
            "PipelineBarrier",
            "vkQueueWaitIdle",
            "DeviceWaitIdle",
        ):
            self.assertNotIn(forbidden, focused)


if __name__ == "__main__":
    unittest.main()
