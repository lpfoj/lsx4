import pathlib
import re
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
VK = ROOT / "funnel-arm" / "src" / "video_core" / "renderer_vulkan"
CACHE_H = (VK / "vk_pipeline_cache.h").read_text(encoding="utf-8")
CACHE = (VK / "vk_pipeline_cache.cpp").read_text(encoding="utf-8")
SERIALIZATION = (VK / "vk_pipeline_serialization.cpp").read_text(
    encoding="utf-8"
)


def cpp_function(source: str, signature: str) -> str:
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 0
    for position in range(brace, len(source)):
        token = source[position]
        if token == "{":
            depth += 1
        elif token == "}":
            depth -= 1
            if depth == 0:
                return source[start : position + 1]
    raise AssertionError(f"unterminated function: {signature}")


class ShaderPermutationInfoOwnershipContractTest(unittest.TestCase):
    def test_each_module_owns_stable_exact_info(self):
        program = CACHE_H[CACHE_H.index("struct Program") : CACHE_H.index("class PipelineCache")]

        self.assertIn("std::unique_ptr<Shader::Info> info", program)
        self.assertGreaterEqual(program.count("spec.info = info.get()"), 2)
        self.assertIn("selected.flattened_ud_buf = std::move(info.flattened_ud_buf)", program)
        for forbidden in ("WaitIdle", "Submit", "mutex", "CUSA"):
            self.assertNotIn(forbidden, cpp_function(program, "ActivatePermut"))

    def test_new_permutation_registers_and_returns_its_compiled_info(self):
        get_program = cpp_function(CACHE, "PipelineCache::GetProgram")

        self.assertIn("CompileModule(*new_info", get_program)
        self.assertIn("RegisterShaderMeta(selected_info", get_program)
        self.assertLess(
            get_program.rindex("ActivatePermut(perm_idx, params)"),
            get_program.rindex("RegisterShaderMeta(selected_info"),
        )
        self.assertIn("std::make_tuple(&selected_info", get_program)
        self.assertNotIn("RegisterShaderMeta(info,", get_program)
        self.assertNotIn("std::make_tuple(&program->info", get_program)

    def test_cache_hits_reuse_selected_info_vector_capacity(self):
        program = CACHE_H[CACHE_H.index("struct Program") : CACHE_H.index("class PipelineCache")]
        cached = cpp_function(program, "ActivateCachedPermut")
        get_program = cpp_function(CACHE, "PipelineCache::GetProgram")

        self.assertIn("selected.flattened_ud_buf.assign", cached)
        self.assertIn("selected.pgm_base = params.Base()", cached)
        self.assertIn("selected.user_data = params.user_data", cached)
        self.assertGreaterEqual(get_program.count("ActivateCachedPermut("), 2)
        self.assertIn("ExecutorCachedPermutFlattenedReuseEnabled()", get_program)
        self.assertIn("ExecutorRecordCachedPermutFlattenedReuse()", get_program)
        self.assertIn("run-enable-cached-permut-flattened-reuse", CACHE)

    def test_cache_load_publishes_the_selected_module_info(self):
        load_stage = cpp_function(SERIALIZATION, "PipelineCache::LoadPipelineStage")

        self.assertIn("std::make_unique<Shader::Info>()", load_stage)
        self.assertIn("std::move(loaded_info)", load_stage)
        self.assertIn("candidate.info && candidate.spec == spec", load_stage)
        self.assertIn("selected_perm_idx = static_cast<size_t>(idx)", load_stage)
        self.assertIn("modules[selected_perm_idx]", load_stage)
        self.assertIn("infos[stage] = selected_module.info.get()", load_stage)
        self.assertNotIn("infos[stage] = &it_pgm.value()->info", load_stage)
        self.assertNotIn("ASSERT_MSG(perm_idx == idx", load_stage)

    def test_old_shared_info_cache_identity_is_invalidated(self):
        binary = re.search(r"ShaderBinaryVersion\s*=\s*(\d+)u", SERIALIZATION)
        meta = re.search(r"ShaderMetaVersion\s*=\s*(\d+)u", SERIALIZATION)

        self.assertIsNotNone(binary)
        self.assertIsNotNone(meta)
        self.assertGreaterEqual(int(binary.group(1)), 31)
        self.assertGreaterEqual(int(meta.group(1)), 2)


if __name__ == "__main__":
    unittest.main()
