import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PIPELINE_CACHE = (
    ROOT / "funnel-arm/src/video_core/renderer_vulkan/vk_pipeline_cache.cpp"
).read_text(encoding="utf-8")
GRAPHICS_PIPELINE = (
    ROOT / "funnel-arm/src/video_core/renderer_vulkan/vk_graphics_pipeline.cpp"
).read_text(encoding="utf-8")


def source_between(source: str, start: str, end: str) -> str:
    return source.split(start, 1)[1].split(end, 1)[0]


class PipelineBuildProfileContractTests(unittest.TestCase):
    def test_gate_is_cached_explicit_marker_only_in_both_translation_units(self):
        for source, clock_name in (
            (PIPELINE_CACHE, "u64 ExecutorGraphicsPipelineLookupProfileNowNs"),
            (GRAPHICS_PIPELINE, "u64 ExecutorGraphicsPipelineBuildProfileNowNs"),
        ):
            gate = source_between(
                source,
                "bool ExecutorGraphicsPipeline",
                clock_name,
            )
            self.assertIn("static const bool enabled", gate)
            self.assertIn("run-diagnostic-pipeline-build-profile", gate)
            self.assertIn("::access", gate)
            self.assertNotIn("getenv", gate)

    def test_normal_lookup_path_does_not_read_clock_or_construct_profile(self):
        body = source_between(
            PIPELINE_CACHE,
            "const GraphicsPipeline* PipelineCache::GetGraphicsPipeline(",
            "const ComputePipeline* PipelineCache::GetComputePipeline()",
        )
        fast_return = body.index(
            "last_graphics_state_generation == state_generation"
        )
        profile_declaration = body.index(
            "std::optional<ExecutorGraphicsPipelineLookupProfile> lookup_profile"
        )
        self.assertLess(fast_return, profile_declaration)
        self.assertIn(
            "const bool lookup_profile_enabled =\n"
            "        ExecutorGraphicsPipelineLookupProfileEnabled();",
            body,
        )
        self.assertIn(
            "if (lookup_profile_enabled && inherited_lookup_profile == nullptr) {\n"
            "        lookup_profile.emplace();",
            body,
        )
        self.assertNotIn("sleep", body)

    def test_refresh_key_and_nested_stage_walk_are_separately_timed(self):
        lookup = source_between(
            PIPELINE_CACHE,
            "const GraphicsPipeline* PipelineCache::GetGraphicsPipeline(",
            "const ComputePipeline* PipelineCache::GetComputePipeline()",
        )
        self.assertIn("ExecutorGraphicsPipelineLookupStage::Refresh", lookup)
        self.assertIn("RefreshGraphicsStages(false)", lookup)
        self.assertIn("RefreshGraphicsKey(host_primitive_override)", lookup)

        stages = source_between(
            PIPELINE_CACHE,
            "bool PipelineCache::RefreshGraphicsStages(",
            "bool PipelineCache::RefreshComputeKey()",
        )
        self.assertIn(
            "ExecutorGraphicsPipelineLookupStage::RefreshStages", stages
        )
        self.assertIn("HleCleanBufferGuestReadScope", stages)

    def test_lookup_reports_lock_negative_map_serialization_and_constructor(self):
        publisher = source_between(
            PIPELINE_CACHE,
            "~ExecutorGraphicsPipelineLookupProfile()",
            "ExecutorGraphicsPipelineLookupProfile(const",
        )
        for field in (
            "backgroundLockUs",
            "refreshUs",
            "refreshStagesUs",
            "negativeLookupUs",
            "mapLookupUs",
            "serializationUs",
            "ctorUs",
            "ctorCalls",
            "mapMiss",
            "otherUs",
        ):
            self.assertIn(field, publisher)
        self.assertIn("SlowLookupThresholdNs = 1'000'000", publisher)
        self.assertEqual(
            PIPELINE_CACHE.count("[EXECUTOR_GRAPHICS_PIPELINE_LOOKUP_PROFILE]"),
            1,
        )

    def test_constructor_splits_layout_driver_calls_and_vk_pipeline_create(self):
        constructor = source_between(
            GRAPHICS_PIPELINE,
            "GraphicsPipeline::GraphicsPipeline(",
            "GraphicsPipeline::~GraphicsPipeline()",
        )
        for call, stage in (
            ("createPipelineLayoutUnique(layout_info)", "PipelineLayout"),
            ("createRenderPassUnique(render_pass_info)", "RenderPass"),
            (
                "createGraphicsPipelineUnique(pipeline_cache, pipeline_info)",
                "VkCreateGraphicsPipelines",
            ),
        ):
            call_index = constructor.index(call)
            begin_index = constructor.rfind("BeginStage()", 0, call_index)
            end_index = constructor.index("EndStage(", call_index)
            self.assertLess(begin_index, call_index)
            self.assertLess(call_index, end_index)
            self.assertIn(stage, constructor[begin_index:end_index + 160])

        descriptor = source_between(
            GRAPHICS_PIPELINE,
            "bool GraphicsPipeline::BuildDescSetLayout(",
            "\n}\n\n}",
        )
        create = descriptor.index("createDescriptorSetLayoutUnique(desc_layout_ci)")
        begin = descriptor.rfind("descriptor_layout_profile_begin", 0, create)
        record = descriptor.index("RecordDescriptorLayout", create)
        self.assertLess(begin, create)
        self.assertLess(create, record)

    def test_build_record_is_observational_and_reports_required_components(self):
        publisher = source_between(
            GRAPHICS_PIPELINE,
            "~ExecutorGraphicsPipelineBuildProfile()",
            "ExecutorGraphicsPipelineBuildProfile(const",
        )
        for field in (
            "totalUs",
            "serializationUs",
            "descriptorBindingsUs",
            "descriptorLayoutUs",
            "pipelineLayoutUs",
            "renderPassUs",
            "vkCreateUs",
            "otherUs",
        ):
            self.assertIn(field, publisher)
        self.assertEqual(
            GRAPHICS_PIPELINE.count("[EXECUTOR_GRAPHICS_PIPELINE_BUILD_PROFILE]"),
            1,
        )
        self.assertNotIn("sleep", publisher)
        self.assertNotIn("createGraphicsPipeline", publisher)
        self.assertNotIn("fopen", publisher)


if __name__ == "__main__":
    unittest.main()
