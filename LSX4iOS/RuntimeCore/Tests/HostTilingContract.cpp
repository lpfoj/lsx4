// SPDX-License-Identifier: GPL-2.0-or-later
// Compile both host tiling routes. This is not a GPU or gameplay benchmark.
#include <glslang/Public/ShaderLang.h>
#include <glslang/Public/ResourceLimits.h>
#include <SPIRV/GlslangToSpv.h>
#include <SPIRV/spirv.hpp>
#include <fstream>
#include <iterator>
#include <iostream>
#include <stdexcept>
#include <vector>

int main(int argc, char** argv) {
    if (argc != 2 || !glslang::InitializeProcess()) return 2;
    try {
        std::ifstream input(argv[1]);
        const std::string source{std::istreambuf_iterator<char>(input), {}};
        if (source.empty()) throw std::runtime_error("Tiling source unavailable");
        unsigned passed = 0;
        for (unsigned bits : {8, 16, 32, 64, 128})
        for (unsigned arrayMode : {2, 4})
        for (bool explicitBounds : {false, true})
        for (bool tiler : {false, true}) {
            const std::string defines =
                "#define BITS_PER_PIXEL " + std::to_string(bits) + "\n"
                "#define NUM_SAMPLES 1\n#define ARRAY_MODE " + std::to_string(arrayMode) + "\n"
                "#define PIPE_CONFIG 12\n#define BANK_WIDTH 1\n#define BANK_HEIGHT 1\n"
                "#define NUM_BANKS 16\n#define NUM_BANK_BITS 4\n"
                "#define TILE_SPLIT_BYTES 512\n#define MACRO_TILE_ASPECT 2\n"
                "#define MICRO_TILE_MODE 2\n#define MICRO_TILE_THICKNESS 1\n" +
                (explicitBounds ? "#define EXPLICIT_TILING_BUFFER_BOUNDS 1\n" : "") +
                (tiler ? "#define IS_TILER 1\n" : "");
            glslang::TShader shader(EShLangCompute);
            const char* text = source.c_str();
            shader.setStrings(&text, 1);
            shader.setPreamble(defines.c_str());
            shader.setEnvInput(glslang::EShSourceGlsl, EShLangCompute, glslang::EShClientVulkan, 450);
            shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_2);
            shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_5);
            const auto messages = EShMessages(EShMsgSpvRules | EShMsgVulkanRules);
            if (!shader.parse(GetDefaultResources(), 450, false, messages))
                throw std::runtime_error(shader.getInfoLog());
            glslang::TProgram program;
            program.addShader(&shader);
            if (!program.link(messages)) throw std::runtime_error(program.getInfoLog());
            std::vector<unsigned> words;
            glslang::SpvOptions options;
            options.disableOptimizer = true;
            glslang::GlslangToSpv(*program.getIntermediate(EShLangCompute), words, &options);
            unsigned arrayLengths = 0, upperBounds = 0;
            for (size_t at = 5; at < words.size();) {
                const auto count = words[at] >> 16, opcode = words[at] & 0xffff;
                if (!count || count > words.size() - at) throw std::runtime_error("Invalid SPIR-V");
                arrayLengths += opcode == spv::OpArrayLength;
                upperBounds += opcode == spv::OpUGreaterThanEqual;
                at += count;
            }
            if (arrayLengths != (explicitBounds ? 0u : 2u) || upperBounds < 2)
                throw std::runtime_error("Tiling route lost array length or index bounds contract");
            ++passed;
        }
        std::cout << "Host tiling contracts: " << passed << " variants passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        glslang::FinalizeProcess();
        return 1;
    }
    glslang::FinalizeProcess();
}
