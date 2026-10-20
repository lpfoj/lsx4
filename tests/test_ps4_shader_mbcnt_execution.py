"""Execute the production MBCNT lowering against an evaluating IR emitter.

This catches operand-dispatch traps and mask arithmetic errors without a GPU.
It deliberately compiles the method extracted from vector_alu.cpp, not a copy
of the implementation. The oracle counts individual GCN lane bits directly.
"""

import os
import pathlib
import shutil
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = ROOT / "funnel-arm/src/shader_recompiler/frontend/translate/vector_alu.cpp"


def compiler_command():
    for name in ("c++", "g++", "clang++"):
        if executable := shutil.which(name):
            return [executable, "-std=c++20", "-O2"], os.environ.copy(), False
    if os.name == "nt":
        vcvars = pathlib.Path(os.environ.get("ProgramFiles", "C:/Program Files")) / (
            "Microsoft Visual Studio/2022/Community/VC/Auxiliary/Build/vcvars64.bat"
        )
        if vcvars.is_file():
            command = f'"{os.environ.get("COMSPEC", "cmd.exe")}" /d /s /c ""{vcvars}" >nul && set"'
            result = subprocess.run(command, check=True, capture_output=True,
                                    text=True, encoding="mbcs")
            environment = os.environ.copy()
            for line in result.stdout.splitlines():
                if "=" in line and not line.startswith("="):
                    key, value = line.split("=", 1)
                    environment[key.upper()] = value
            compiler = shutil.which("cl.exe", path=environment.get("PATH"))
            if compiler:
                return [compiler, "/nologo", "/std:c++20", "/EHsc", "/O2"], environment, True
    raise unittest.SkipTest("A host C++20 compiler is required for the MBCNT execution test")


PREAMBLE = r"""
#include <array>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <stdexcept>
using u32 = std::uint32_t;
using u64 = std::uint64_t;
enum class Stage { Compute, Vertex };
enum class OperandField { ScalarGPR, VectorGPR, LiteralConst, ConstZero,
                          SignedConstIntPos, SignedConstIntNeg, ExecLo, ExecHi };
struct InstOperand { OperandField field; u32 code; };
struct GcnInst { std::array<InstOperand, 2> src; std::array<InstOperand, 1> dst; };
namespace IR {
using U32 = u32;
using U1 = bool;
enum class Attribute { LocalInvocationId };
}
struct Emitter {
    u64 exec_mask{};
    u32 lane{};
    std::array<u32, 3> local{};
    u32 ballots{};
    IR::U32 Imm32(u32 x) { return x; }
    IR::U32 LaneId() { return lane; }
    IR::U32 GetAttributeU32(IR::Attribute, u32 component) { return local[component]; }
    IR::U32 BitwiseAnd(u32 a, u32 b) { return a & b; }
    IR::U32 ISub(u32 a, u32 b) { return a - b; }
    IR::U32 IAdd(u32 a, u32 b) { return a + b; }
    IR::U32 IMul(u32 a, u32 b) { return a * b; }
    IR::U32 ShiftLeftLogical(u32 a, u32 b) {
        if (b >= 32) throw std::runtime_error("undefined-width shift");
        return a << b;
    }
    IR::U1 ILessThan(u32 a, u32 b, bool) { return a < b; }
    IR::U32 Select(bool predicate, u32 a, u32 b) { return predicate ? a : b; }
    IR::U32 BitCount(u32 value) { return std::popcount(value); }
    IR::U1 GetExec() { return ((exec_mask >> lane) & 1) != 0; }
    std::array<u32, 4> Ballot(bool) {
        ++ballots;
        return {u32(exec_mask), u32(exec_mask >> 32), 0, 0};
    }
    IR::U32 CompositeExtract(const std::array<u32, 4>& value, u32 index) {
        return value.at(index);
    }
};
struct Translator {
    Emitter ir;
    struct {
        Stage stage;
        struct { std::array<u32, 3> workgroup_size; } cs_info;
    } runtime_info{};
    u32 destination{};
    // Generic data operand decoding models GetSrc. EXEC must never reach it:
    // the production GetSrc has no integer EXEC case, which caused the crash.
    IR::U32 GetSrc(const InstOperand& operand) {
        switch (operand.field) {
        case OperandField::ConstZero: return 0;
        case OperandField::SignedConstIntPos: return operand.code - 129 + 1;
        case OperandField::SignedConstIntNeg: return u32(-std::int32_t(operand.code) + 193 - 1);
        case OperandField::ScalarGPR:
        case OperandField::VectorGPR:
        case OperandField::LiteralConst: return operand.code;
        default: throw std::runtime_error("EXEC reached unsupported generic GetSrc");
        }
    }
    void SetDst(const InstOperand&, u32 value) { destination = value; }
    void V_MBCNT_U32_B32(bool is_low, const GcnInst& inst);
};
"""

HARNESS = r"""
u32 operand_value(InstOperand operand, u64 exec) {
    switch (operand.field) {
    case OperandField::ExecLo: return u32(exec);
    case OperandField::ExecHi: return u32(exec >> 32);
    case OperandField::ConstZero: return 0;
    case OperandField::SignedConstIntPos: return operand.code - 128;
    case OperandField::SignedConstIntNeg: return ~u32(operand.code - 193);
    default: return operand.code;
    }
}
bool is_exec(InstOperand operand) {
    return operand.field == OperandField::ExecLo || operand.field == OperandField::ExecHi;
}
int main() {
    const std::array<u64, 7> masks{0, ~u64(0), 1, u64(1) << 31, u64(1) << 32,
                                 u64(1) << 63, 0xA53C96E78124F0B5ULL};
    const std::array<InstOperand, 11> operands{{
        {OperandField::ConstZero, 128}, {OperandField::SignedConstIntNeg, 193},
        {OperandField::SignedConstIntNeg, 208}, {OperandField::SignedConstIntPos, 192},
        {OperandField::LiteralConst, 0x80000001}, {OperandField::ScalarGPR, 0x1234A5A5},
        {OperandField::VectorGPR, 0xFFFFFFFF}, {OperandField::ExecLo, 126},
        {OperandField::ExecHi, 127}, {OperandField::LiteralConst, 0xFFFFFFF0},
        {OperandField::LiteralConst, 17}
    }};
    const std::array<std::array<u32, 3>, 4> shapes{{{64,1,1}, {8,4,2}, {3,5,7}, {128,1,1}}};
    u64 cases{};
    for (const auto stage : {Stage::Compute, Stage::Vertex}) {
        for (const auto shape : shapes) {
            for (const u64 mask : masks) {
                for (u32 linear = 0; linear < shape[0] * shape[1] * shape[2]; ++linear) {
                    const u32 lane = linear % 64;
                    // An inactive lane doesn't execute this VALU instruction.
                    if ((mask & (u64(1) << lane)) == 0) continue;
                    for (const bool low : {false, true}) {
                        for (const auto source0 : operands) {
                            for (const auto source1 : operands) {
                                Translator translator;
                                translator.ir.exec_mask = mask;
                                translator.ir.lane = lane;
                                translator.ir.local = {linear % shape[0],
                                    (linear / shape[0]) % shape[1], linear / (shape[0] * shape[1])};
                                translator.runtime_info.stage = stage;
                                translator.runtime_info.cs_info.workgroup_size = shape;
                                GcnInst inst{{source0, source1}, {{{OperandField::VectorGPR, 0}}}};
                                translator.V_MBCNT_U32_B32(low, inst);
                                const u32 input = operand_value(source0, mask);
                                u32 expected = operand_value(source1, mask);
                                // ISA oracle: count individual source-half bits whose
                                // absolute wave64 lane precedes the current lane.
                                for (u32 bit = 0; bit < 32; ++bit) {
                                    const u32 absolute = bit + (low ? 0 : 32);
                                    if (absolute < lane && (input & (u32(1) << bit))) ++expected;
                                }
                                if (translator.destination != expected ||
                                    translator.ir.ballots != u32(is_exec(source0)) + u32(is_exec(source1))) {
                                    std::fprintf(stderr, "mismatch lane=%u low=%u got=%u expected=%u\n",
                                                 lane, low, translator.destination, expected);
                                    return 1;
                                }
                                ++cases;
                            }
                        }
                    }
                }
            }
        }
    }
    std::printf("MBCNT execution cases=%llu PASS\n", static_cast<unsigned long long>(cases));
}
"""


class MbcntExecutionTests(unittest.TestCase):
    def test_production_lowering_against_lane_bit_oracle(self):
        source = SOURCE.read_text(encoding="utf-8")
        begin = source.index("void Translator::V_MBCNT_U32_B32")
        end = source.index("void Translator::V_ADD_I32", begin)
        command, environment, msvc = compiler_command()
        with tempfile.TemporaryDirectory(prefix="lsx4-mbcnt-") as temporary:
            directory = pathlib.Path(temporary)
            cpp = directory / "mbcnt.cpp"
            executable = directory / ("mbcnt.exe" if os.name == "nt" else "mbcnt")
            cpp.write_text(PREAMBLE + source[begin:end] + HARNESS, encoding="utf-8")
            arguments = command + [str(cpp)]
            arguments += [f"/Fe:{executable}"] if msvc else ["-o", str(executable)]
            compiled = subprocess.run(arguments, cwd=directory, env=environment,
                                      text=True, capture_output=True)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            result = subprocess.run([str(executable)], text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("PASS", result.stdout)
            print(result.stdout.strip())


if __name__ == "__main__":
    unittest.main()
