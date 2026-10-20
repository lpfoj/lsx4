"""Execute production compute constructor arguments with distinct stage slots.

Shader/module arrays are compact in this path, but runtime arrays are indexed
by logical stage. Compile the real call expressions and capability gate so a
fragment union accidentally supplied as a compute workgroup fails this test.
"""

import os
import pathlib
import re
import subprocess
import tempfile
import unittest

from tests.test_pipeline_warmup_runtime_info_contract import cpp_function
from tests.test_ps4_shader_mbcnt_execution import compiler_command


ROOT = pathlib.Path(__file__).resolve().parents[1]
VK = ROOT / "funnel-arm/src/video_core/renderer_vulkan"


def calls(source, marker):
    result = []
    position = 0
    while (start := source.find(marker, position)) >= 0:
        opening = source.index("(", start + len(marker))
        depth = 1
        end = opening + 1
        while depth:
            depth += (source[end] == "(") - (source[end] == ")")
            end += 1
        result.append(source[start:end])
        position = end
    return result


PREAMBLE = r"""
#include <array>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <stdexcept>
using u32 = std::uint32_t;
using u64 = std::uint64_t;
namespace vk {
struct ShaderStageFlagBits { static constexpr u32 eCompute = 1; };
struct PipelineCreateFlagBits {
    static constexpr int eFailOnPipelineCompileRequired = 1;
};
}
struct RuntimeInfo { struct { std::array<u32, 3> workgroup_size; } cs_info; };
struct Info { int identity; };
struct Instance {
    struct { bool subgroupSizeControl = true; } vk13_features;
    struct {
        u32 minSubgroupSize = 32, maxSubgroupSize = 64;
        u32 requiredSubgroupSizeStages = 1, maxComputeWorkgroupSubgroups = 16;
    } vk13_props;
"""

FIXTURES = r"""
};
std::array<u32, 3> expected;
bool expected_supported;
u32 checked_calls;
void check(Instance& instance, const Info& info,
           const std::array<u32, 3>& workgroup, int module) {
    if (info.identity != 73 || module != 91 || workgroup != expected ||
        instance.IsSubgroupSize64Supported(workgroup) != expected_supported)
        throw std::runtime_error("compact info/runtime stage or subgroup gate mismatch");
    ++checked_calls;
}
struct ComputePipeline {
    struct SerializationSupport {};
    ComputePipeline(Instance& instance, int, int, int, int, int,
                    const Info& info, const std::array<u32, 3>& workgroup,
                    int module, SerializationSupport&, bool, int = 0) {
        check(instance, info, workgroup, module);
    }
};
int ForegroundFallbackBuildFlags(const Instance&) { return 2; }
struct Payload { std::array<u32, 3> workgroup_size; } queued;
Instance queue_instance;
void QueueAsyncComputePipeline(int, const Info& info,
    const std::array<u32, 3>& workgroup_size, int module,
    ComputePipeline::SerializationSupport, bool) {
    auto* payload = &queued;
    __PAYLOAD_COPY__
    check(queue_instance, info, payload->workgroup_size, module);
}
int main() {
    struct Case { std::array<u32, 3> shape; bool supported; };
    const std::array<Case, 8> cases{{
        {{64,1,1},true}, {{8,4,2},true}, {{3,5,7},true}, {{1,1,1},true},
        {{1024,1,1},true}, {{1025,1,1},false}, {{0,4,2},false},
        {{~u32(0),~u32(0),~u32(0)},false}
    }};
    for (const auto& item : cases) {
      for (const auto poison : {std::array<u32,3>{2,6,0},
                                std::array<u32,3>{770,0,0}}) {
        expected = item.shape;
        expected_supported = item.supported;
        Instance instance;
        int scheduler{}, desc_heap{}, profile{}, cache{};
        int* pipeline_cache = &cache;
        int* local_cache = &cache;
        int compute_key{}, key{};
        Info compact_info{73}, unrelated_info{99};
        std::array<Info*, 6> infos{}, warm_infos{};
        infos[0] = warm_infos[0] = &compact_info;
        infos[5] = warm_infos[5] = &unrelated_info;
        std::array<int,6> modules{}, warm_modules{};
        modules[0] = warm_modules[0] = 91;
        modules[5] = warm_modules[5] = 99;
        ComputePipeline::SerializationSupport sdata{};
        std::array<RuntimeInfo,6> runtime_infos{}, warm_runtime_infos{};
        for (auto& runtime : runtime_infos) runtime.cs_info.workgroup_size = poison;
        for (auto& runtime : warm_runtime_infos) runtime.cs_info.workgroup_size = poison;
        runtime_infos[static_cast<u32>(Shader::LogicalStage::Compute)]
            .cs_info.workgroup_size = item.shape;
        __LIVE_SELECTION__
        __LIVE_CALLS__
        runtime_infos[static_cast<u32>(Shader::LogicalStage::Compute)]
            .cs_info.workgroup_size = poison;
        if (queued.workgroup_size != expected)
            throw std::runtime_error("async workgroup did not retain its value");
        struct { RuntimeInfo runtime_info; } spec{{{item.shape}}};
        // Warmup's compact stage argument is 0; runtime storage remains logical.
        const size_t stage = 0;
        __LOAD_RUNTIME__
        __LOAD_CALL__;
        __WARM_RUNTIME__
        __WARM_CALL__;
      }
    }
    if (checked_calls != 96) throw std::runtime_error("a pipeline route was omitted");
    std::printf("compute runtime routes=%u PASS\n", checked_calls);
}
"""


class ComputeRuntimeStageExecutionTests(unittest.TestCase):
    def test_all_compute_routes_use_logical_runtime_and_compact_info(self):
        cache = (VK / "vk_pipeline_cache.cpp").read_text(encoding="utf-8")
        serialization = (VK / "vk_pipeline_serialization.cpp").read_text(encoding="utf-8")
        instance = (VK / "vk_instance.h").read_text(encoding="utf-8")
        runtime = (ROOT / "funnel-arm/src/shader_recompiler/runtime_info.h").read_text(
            encoding="utf-8")
        logical_enum = re.search(r"enum class LogicalStage : u32 \{.*?\};", runtime,
                                 re.DOTALL).group()
        gate = cpp_function(instance, "bool IsSubgroupSize64Supported(")
        live = cpp_function(cache, "const ComputePipeline* PipelineCache::GetComputePipeline")
        constructors = calls(live, "std::make_unique<ComputePipeline>")
        asynchronous = calls(live, "QueueAsyncComputePipeline")
        self.assertEqual(len(constructors), 3)
        self.assertEqual(len(asynchronous), 1)
        selection = re.search(r"const auto workgroup_size\s*=.*?;", live, re.DOTALL).group()
        self.assertLess(live.index("RefreshComputeKey()"), live.index(selection))
        queue = cpp_function(cache, "bool PipelineCache::QueueAsyncComputePipeline")
        payload_copy = re.search(r"payload->workgroup_size\s*=.*?;", queue).group()
        load = cpp_function(serialization, "bool PipelineCache::LoadComputePipeline")
        warm = cpp_function(serialization, "bool PipelineCache::LoadIsolatedWarmUpPipeline")
        load_stage = cpp_function(serialization, "bool PipelineCache::LoadPipelineStage")
        source = (PREAMBLE + gate + FIXTURES).replace(
            "struct RuntimeInfo", f"namespace Shader {{ {logical_enum} }}\nstruct RuntimeInfo", 1)
        replacements = {
            "__PAYLOAD_COPY__": payload_copy,
            "__LIVE_SELECTION__": selection,
            "__LIVE_CALLS__": ";\n".join(constructors + asynchronous) + ";",
            "__LOAD_RUNTIME__": re.search(r"runtime_infos\[.*?= spec.runtime_info;",
                                           load_stage, re.DOTALL).group(),
            "__LOAD_CALL__": calls(load, "std::make_unique<ComputePipeline>")[0],
            "__WARM_RUNTIME__": re.search(r"warm_runtime_infos\[.*?= spec.runtime_info;",
                                           warm, re.DOTALL).group(),
            "__WARM_CALL__": calls(warm, "std::make_unique<ComputePipeline>")[0],
        }
        for token, value in replacements.items():
            source = source.replace(token, value)
        command, environment, msvc = compiler_command()
        with tempfile.TemporaryDirectory(prefix="lsx4-compute-stage-") as temporary:
            directory = pathlib.Path(temporary)
            cpp = directory / "stage.cpp"
            executable = directory / ("stage.exe" if os.name == "nt" else "stage")
            cpp.write_text(source, encoding="utf-8")
            arguments = command + [str(cpp)]
            arguments += [f"/Fe:{executable}"] if msvc else ["-o", str(executable)]
            compiled = subprocess.run(arguments, cwd=directory, env=environment,
                                      text=True, capture_output=True)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            result = subprocess.run([str(executable)], text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("routes=96 PASS", result.stdout)
            print(result.stdout.strip())

    def test_exec_codegen_revision_is_in_shader_and_pipeline_identity(self):
        source = (VK / "vk_pipeline_serialization.cpp").read_text(encoding="utf-8")
        version = int(re.search(r"ShaderBinaryVersion = (\d+)u", source).group(1))
        self.assertGreaterEqual(version, 33)
        for signature in ("std::vector<u8> ShaderStorageIdentity",
                          "std::string PipelineStorageNamespace"):
            self.assertIn("Serialization::ShaderBinaryVersion", cpp_function(source, signature))


if __name__ == "__main__":
    unittest.main()
