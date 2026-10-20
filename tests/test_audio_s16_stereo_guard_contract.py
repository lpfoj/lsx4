from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
AUDIO_MIX = (
    ROOT / "funnel-arm/src/core/libraries/audio/audio_mix.cpp"
).read_text(encoding="utf-8")


def function_body(source: str, signature: str) -> str:
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 0
    for index in range(opening, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[opening + 1 : index]
    raise AssertionError(f"unterminated function: {signature}")


class AudioS16StereoGuardContract(unittest.TestCase):
    def test_runtime_guard_covers_exact_s16_stereo_1024_shape(self) -> None:
        guard = function_body(AUDIO_MIX, "bool RunRegressionFixtures")
        start = guard.index("constexpr u32 StereoFrames = 1024")
        end = guard.index("std::vector<float> stereo_float", start)
        s16_fixture = guard[start:end]

        self.assertIn("std::vector<s16> stereo_s16", s16_fixture)
        self.assertIn(".sample_type = AudioSampleType::S16", s16_fixture)
        self.assertIn(".channels = 2", s16_fixture)
        self.assertIn("std::numeric_limits<s16>::min()", s16_fixture)
        self.assertIn("std::numeric_limits<s16>::max()", s16_fixture)
        self.assertIn("ConvertScalar(stereo_s16.data(), StereoFrames", s16_fixture)
        self.assertIn("ConvertSimd(stereo_s16.data(), StereoFrames", s16_fixture)
        self.assertIn("FloatBuffersEqual(scalar_stereo, optimized_stereo)", s16_fixture)

    def test_s16_guard_precedes_float_guard_and_stream_checks(self) -> None:
        guard = function_body(AUDIO_MIX, "bool RunRegressionFixtures")
        s16 = guard.index("const AudioBufferFormat s16_stereo")
        float_fixture = guard.index("const AudioBufferFormat float_stereo")
        stream = guard.index("AudioStreamGuard stream_guard")
        self.assertLess(s16, float_fixture)
        self.assertLess(float_fixture, stream)


if __name__ == "__main__":
    unittest.main()
