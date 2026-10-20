import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
VK = ROOT / "funnel-arm" / "src" / "video_core" / "renderer_vulkan"


class RequiredSubgroupSizeContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.instance = (VK / "vk_instance.h").read_text(encoding="utf-8")
        cls.instance_cpp = (VK / "vk_instance.cpp").read_text(encoding="utf-8")
        cls.capability = (VK / "vk_capability_profile.h").read_text(
            encoding="utf-8"
        )
        cls.compute = (VK / "vk_compute_pipeline.cpp").read_text(encoding="utf-8")
        cls.compute_h = (VK / "vk_compute_pipeline.h").read_text(encoding="utf-8")
        cls.cache = (VK / "vk_pipeline_cache.cpp").read_text(encoding="utf-8")
        cls.cache_h = (VK / "vk_pipeline_cache.h").read_text(encoding="utf-8")
        cls.serialization = (VK / "vk_pipeline_serialization.cpp").read_text(
            encoding="utf-8"
        )
        cls.info = (
            ROOT / "funnel-arm" / "src" / "shader_recompiler" / "info.h"
        ).read_text(encoding="utf-8")
        cls.collection = (
            ROOT
            / "funnel-arm"
            / "src"
            / "shader_recompiler"
            / "ir"
            / "passes"
            / "shader_info_collection_pass.cpp"
        ).read_text(encoding="utf-8")

    def test_wave64_gate_covers_every_required_vulkan_capability(self):
        gate = self.instance.split("bool IsSubgroupSize64Supported(", 1)[1].split(
            "bool IsPipelineCreationCacheControlSupported", 1
        )[0]
        self.assertIn("vk13_features.subgroupSizeControl", gate)
        self.assertIn("vk13_props.minSubgroupSize > RequiredSubgroupSize", gate)
        self.assertIn("vk13_props.maxSubgroupSize < RequiredSubgroupSize", gate)
        self.assertIn("vk13_props.requiredSubgroupSizeStages", gate)
        self.assertIn("vk::ShaderStageFlagBits::eCompute", gate)
        self.assertIn("vk13_props.maxComputeWorkgroupSubgroups", gate)

    def test_wave64_gate_checks_the_real_workgroup_without_overflow(self):
        gate = self.instance.split("bool IsSubgroupSize64Supported(", 1)[1].split(
            "bool IsPipelineCreationCacheControlSupported", 1
        )[0]
        self.assertIn("const std::array<u32, 3>& workgroup_size", gate)
        self.assertIn("dimension == 0", gate)
        self.assertIn(
            "workgroup_invocations > max_workgroup_invocations / dimension", gate
        )
        self.assertIn("workgroup_invocations *= dimension", gate)
        self.assertIn(
            "instance.IsSubgroupSize64Supported(workgroup_size)", self.compute
        )

    def test_unsupported_wave64_keeps_the_existing_pipeline_stage_path(self):
        stage = self.compute.split(
            "const vk::PipelineShaderStageCreateInfo shader_ci", 1
        )[1].split("u32 binding", 1)[0]
        self.assertIn("require_subgroup_size_64 ? &subgroup_size_ci : nullptr", stage)
        self.assertNotIn("REQUIRE_FULL_SUBGROUPS", stage)
        for forbidden in ("Finish(", "Wait(", "PipelineBarrier", "CUSA"):
            self.assertNotIn(forbidden, self.compute)

    def test_a750_qualcomm_keeps_baseline_required_subgroup_size_after_failed_ab(self):
        quirk = "AvoidRequiredSubgroupSizeForSubgroupIndependentCompute"
        self.assertIn(quirk, self.capability)
        self.assertIn("ExactProprietaryAdreno750PipelineQuirks", self.instance_cpp)
        self.assertIn("bool uses_subgroup_ops{}", self.info)
        self.assertIn("!info->uses_subgroup_ops", self.compute)
        self.assertIn("!avoid_redundant_required_subgroup_size", self.compute)



        route = self.capability.split(
            "constexpr u64 ExactProprietaryAdreno750PipelineQuirks", 1
        )[1].split("constexpr u64 ExactProprietaryAdreno840PipelineQuirks", 1)[0]
        self.assertIn("IsExactProprietaryAdreno750", route)
        returned_quirks = route.rsplit("return ", 1)[1]
        self.assertNotIn(quirk, returned_quirks)
        self.assertIn("failed subgroup-size", route)

    def test_a840_qualcomm_omits_only_redundant_required_subgroup_size(self):
        quirk = "AvoidRequiredSubgroupSizeForSubgroupIndependentCompute"
        route = self.capability.split(
            "constexpr u64 ExactProprietaryAdreno840PipelineQuirks", 1
        )[1].split("constexpr u64 ExactProprietaryAdreno750Quirks", 1)[0]
        self.assertIn("IsExactProprietaryAdreno840", route)
        self.assertIn(quirk, route)
        self.assertIn("Adreno840DeviceId = 0x44050a31", self.capability)
        self.assertIn("ExactProprietaryAdreno840PipelineQuirks", self.instance_cpp)
        self.assertIn("!info->uses_subgroup_ops", self.compute)
        self.assertIn("!avoid_redundant_required_subgroup_size", self.compute)

        guards = self.capability.split(
            "static_assert(IsExactProprietaryAdreno840", 1
        )[1]
        self.assertIn("MobileDriverFamily::MesaTurnip", guards)
        self.assertIn("MobileDriverFamily::ArmProprietary", guards)
        self.assertIn("Adreno750DeviceId", guards)

    def test_subgroup_guard_covers_every_emitted_warp_operation(self):
        for opcode in (
            "LaneId",
            "QuadShuffle",
            "ReadFirstLane",
            "ReadLane",
            "WriteLane",
            "Ballot",
            "BallotFindLsb",
            "GroupAny",
        ):
            case = self.collection.split(f"case IR::Opcode::{opcode}:", 1)[1]
            case = case.split("break;", 1)[0]
            self.assertIn("info.uses_subgroup_ops = true", case, opcode)

    def test_cached_spirv_recovers_subgroup_usage_before_pipeline_warmup(self):
        self.assertIn("SpirvUsesSubgroupSemantics", self.serialization)
        self.assertIn("OpGroupNonUniformElect", self.serialization)
        self.assertIn("BuiltInSubgroupLocalInvocationId", self.serialization)
        self.assertGreaterEqual(
            self.serialization.count(
                "RestoreSerializedSubgroupUsageForPipelineQuirk"
            ),
            3,
        )

    def test_live_and_async_pipeline_paths_carry_the_same_workgroup_size(self):
        self.assertIn("std::array<u32, 3> workgroup_size{}", self.cache_h)
        self.assertIn("payload->workgroup_size = workgroup_size", self.cache)
        self.assertIn("compute->workgroup_size", self.cache)
        self.assertNotIn("runtime_infos[0].cs_info.workgroup_size", self.cache)
        self.assertIn(
            "runtime_infos[static_cast<u32>(Shader::LogicalStage::Compute)].cs_info.workgroup_size",
            self.cache,
        )

    def test_serialized_warmup_recovers_compute_runtime_info(self):
        self.assertIn(
            "runtime_infos[static_cast<u32>(Shader::LogicalStage::Compute)] = spec.runtime_info",
            self.serialization,
        )
        self.assertIn(
            "warm_runtime_infos[static_cast<u32>(Shader::LogicalStage::Compute)] = spec.runtime_info",
            self.serialization,
        )
        self.assertGreaterEqual(
            self.serialization.count("cs_info.workgroup_size"), 2
        )


if __name__ == "__main__":
    unittest.main()
