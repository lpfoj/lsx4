from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
AUDIOOUT = (
    ROOT / "funnel-arm/src/core/libraries/audio/audioout.cpp"
).read_text(encoding="utf-8")
AAUDIO = (
    ROOT / "funnel-arm/src/core/libraries/audio/sdl_audio_out.cpp"
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


class AAudioFirstNonzeroS16ProbeContract(unittest.TestCase):
    def test_exact_selector_is_shared_by_slot_and_backend_probes(self) -> None:
        for source in (AUDIOOUT, AAUDIO):
            self.assertIn('"s16-2ch-48k-1024"', source)
            self.assertIn("FirstS16Stereo48k1024", source)

        slot_match = function_body(AUDIOOUT, "static bool AndroidAudioProbeMatches")
        for predicate in (
            "!port.format_info.is_float",
            "port.format_info.sample_size == sizeof(s16)",
            "port.format_info.num_channels == 2",
            "port.sample_rate == 48000",
            "port.buffer_frames == 1024",
        ):
            self.assertIn(predicate, slot_match)

        backend_probe = function_body(AAUDIO, "u32 ProbeRawGuestPcm")
        for predicate in (
            "!is_float",
            "guest_sample_size == sizeof(s16)",
            "guest_channels == 2",
            "sample_rate == 48000",
            "buffer_frames == 1024",
        ):
            self.assertIn(predicate, backend_probe)

    def test_targeted_slot_probe_waits_for_content_and_captures_once(self) -> None:
        copy_probe = function_body(AUDIOOUT, "static void AndroidAudioProbeCopy")
        silence_gate = copy_probe.index("std::all_of(bytes, bytes + size")
        first_ordinal = copy_probe.index("const u64 ordinal")
        self.assertLess(silence_gate, first_ordinal)
        self.assertIn("const u64 probe_limit = first_s16_block ? 1 : 128", copy_probe)
        self.assertIn("if (ordinal >= probe_limit)", copy_probe)
        self.assertIn("port.diagnostic_probe_exhausted = true", copy_probe)

    def test_backend_budget_is_content_gated_and_one_block_in_targeted_mode(self) -> None:
        raw_probe = function_body(AAUDIO, "u32 ProbeRawGuestPcm")
        self.assertIn(
            "raw_probe_budget.store(first_s16_block ? 1 : 128",
            raw_probe,
        )
        self.assertLess(
            raw_probe.index("if (!any_nonzero)"),
            raw_probe.index("raw_probe_budget.compare_exchange_weak"),
        )

    def test_same_probe_id_crosses_raw_conversion_and_write_stages(self) -> None:
        backend = AAUDIO.index("class AndroidAAudioPortBackend")
        output_start = AAUDIO.index("void Output(void* ptr) override", backend)
        output = AAUDIO[output_start : AAUDIO.index("void OutputFloat", output_start)]
        raw = output.index("const u32 probe_id = ProbeRawGuestPcm(ptr)")
        convert = output.index("ConvertToOutput(ptr)")
        post = output.index("ProbePostStereo(probe_id)")
        submit = output.index("SubmitOutputBuffer(any_nonzero, probe_id)")
        self.assertLess(raw, convert)
        self.assertLess(convert, post)
        self.assertLess(post, submit)

    def test_flow_marker_is_emitted_after_the_bounded_write(self) -> None:
        submit = function_body(AAUDIO, "void SubmitOutputBuffer")
        write = submit.index("AAudioStream_write")
        after_counter = submit.index("AAudioStream_getFramesWritten(stream)", write)
        flow = submit.index("[EXECUTOR_AUDIO_AAUDIO_FLOW]")
        self.assertLess(write, after_counter)
        self.assertLess(after_counter, flow)
        for field in (
            "completed=",
            "writtenDelta=",
            "readDelta=",
            "sampleRate=",
            "sourceChannels=",
            "sourceSample=",
        ):
            self.assertIn(field, submit)


if __name__ == "__main__":
    unittest.main()
