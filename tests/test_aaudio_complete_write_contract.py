import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = (
    ROOT
    / "funnel-arm"
    / "src"
    / "core"
    / "libraries"
    / "audio"
    / "sdl_audio_out.cpp"
).read_text(encoding="utf-8")


class AAudioCompleteWriteContract(unittest.TestCase):
    def test_partial_writes_advance_the_interleaved_source(self):
        self.assertIn(
            "while (total_written < static_cast<int32_t>(buffer_frames))", SOURCE
        )
        self.assertIn(
            "output_buffer.data() + static_cast<size_t>(total_written) * output_channels",
            SOURCE,
        )
        self.assertIn("total_written += written", SOURCE)

    def test_timeout_covers_a_full_guest_block_without_spinning(self):
        self.assertIn("period_ns * 2", SOURCE)
        self.assertIn("if (written <= 0)", SOURCE)
        self.assertIn("break;", SOURCE)

    def test_raw_pcm_probe_is_before_conversion_and_strictly_bounded(self):
        backend = SOURCE.index("class AndroidAAudioPortBackend")
        output = SOURCE.index("void Output(void* ptr) override", backend)
        direct_output = SOURCE[
            output : SOURCE.index("void OutputFloat", output)
        ]
        self.assertLess(
            direct_output.index("ProbeRawGuestPcm(ptr)"),
            direct_output.index("ConvertToOutput(ptr)"),
        )
        self.assertIn("std::atomic_int raw_probe_budget{4}", SOURCE)
        self.assertIn(
            "if (raw_probe_budget.load(std::memory_order_relaxed) <= 0)", SOURCE
        )
        self.assertIn("constexpr u32 PatternFrames = 8", SOURCE)

    def test_probe_is_warn_only_and_records_raw_and_post_conversion_stats(self):
        self.assertIn('ANDROID_LOG_WARN, "LSX4AudioProbe"', SOURCE)
        for marker in (
            "[EXECUTOR_AUDIO_RAW_PCM]",
            "[EXECUTOR_AUDIO_RAW_PCM_LANE]",
            "[EXECUTOR_AUDIO_RAW_PCM_BITS]",
            "[EXECUTOR_AUDIO_POST_STEREO]",
            "[EXECUTOR_AUDIO_POST_STEREO_LANE]",
        ):
            self.assertIn(marker, SOURCE)
        for field in (
            "peak=",
            "rms=",
            "dc=",
            "nonFinite=",
            "clipped=",
            "zero=",
            "subnormal=",
        ):
            self.assertIn(field, SOURCE)


if __name__ == "__main__":
    unittest.main()
