from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (
    ROOT / "funnel-arm/src/video_core/renderer_vulkan/vk_pipeline_cache.cpp"
).read_text(encoding="utf-8")
HEADER = (
    ROOT / "funnel-arm/src/video_core/renderer_vulkan/vk_pipeline_cache.h"
).read_text(encoding="utf-8")


class RuntimeInfoIdentityContractTests(unittest.TestCase):
    def test_hot_path_keeps_runtime_info_by_reference(self):
        get_program = SOURCE.split("PipelineCache::GetProgram(", 1)[1]
        self.assertIn("const auto& runtime_info = *runtime_info_source", get_program)
        self.assertNotIn("auto runtime_info = *runtime_info_source", get_program)
        self.assertIn("auto compilation_runtime_info = runtime_info", get_program)
        self.assertIn("compilation_runtime_info.emplace(runtime_info)", get_program)

    def test_mutable_translation_copy_is_preserved_only_on_compile(self):
        self.assertIn("Shader::RuntimeInfo& runtime_info", HEADER)
        self.assertIn(
            "CompileModule(program->info, compilation_runtime_info", SOURCE
        )
        self.assertIn(
            "CompileModule(*new_info, *compilation_runtime_info", SOURCE
        )
        self.assertIn("const Shader::RuntimeInfo& snapshot_runtime_info", SOURCE)

    def test_android_cache_entries_use_compact_identity(self):
        hot_entry = SOURCE.split("struct ProgramHotEntry", 1)[1].split(
            "// Key this cache", 1
        )[0]
        self.assertIn("u64 runtime_info_identity", hot_entry)
        self.assertNotIn("Shader::RuntimeInfo runtime_info", hot_entry)
        self.assertIn("u64 runtime_info_identity", HEADER)
        self.assertIn(
            "snapshot.runtime_info_identity == runtime_info_identity", SOURCE
        )

    def test_hash_collision_requires_exact_byte_match(self):
        intern = SOURCE.split("PipelineCache::InternRuntimeInfoIdentity", 1)[1].split(
            "PipelineCache::PipelineCache", 1
        )[0]
        self.assertIn("XXH3_64bits(&info, sizeof(info))", intern)
        self.assertIn("candidate.hash == hash", intern)
        self.assertIn(
            "std::memcmp(&candidate.info, &info, sizeof(info)) == 0", intern
        )
        self.assertIn("victim.identity = ++runtime_info_identity_sequence", intern)

    def test_eviction_is_set_associative_and_only_changes_identity(self):
        self.assertIn("RuntimeInfoIdentitySetCount = 16", HEADER)
        self.assertIn("RuntimeInfoIdentityWayCount = 4", HEADER)
        self.assertIn("runtime_info_identity_next_way", HEADER)
        self.assertIn("std::memcpy(&victim.info, &info, sizeof(info))", SOURCE)

    def test_runtime_witness_states_exact_contract(self):
        self.assertIn("[EXECUTOR_RUNTIME_INFO_IDENTITY] firstHit=1", SOURCE)
        self.assertIn("collisionResolved=1 fullHotCompare=0", SOURCE)
        self.assertIn("runtimeInfoBytes=%zu tlsBytesSaved=%zu", SOURCE)
        self.assertIn("runtime-info-identity-hit.txt", SOURCE)


if __name__ == "__main__":
    unittest.main()
