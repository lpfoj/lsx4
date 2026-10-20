from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (
    ROOT / "funnel-arm/src/core/libraries/audio/sdl_audio_out.cpp"
).read_text(encoding="utf-8")
RAW_CAPTURE = SOURCE[
    SOURCE.index("class AndroidRawPcmCapture final") : SOURCE.index(
        "class AndroidAAudioPortBackend"
    )
]
BACKEND = SOURCE[SOURCE.index("class AndroidAAudioPortBackend") :]


def method_body(source: str, signature: str) -> str:
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 0
    for index in range(opening, len(source)):
        char = source[index]
        if char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return source[opening + 1 : index]
    raise AssertionError(f"unterminated method: {signature}")


class AAudioRawPcmCaptureContract(unittest.TestCase):
    def test_capture_is_explicitly_property_gated(self) -> None:
        requested = method_body(SOURCE, "static bool AndroidRawPcmCaptureRequested")
        start = method_body(BACKEND, "void StartRawPcmCapture")
        self.assertIn('"debug.lsx4.audio_raw_capture"', SOURCE)
        self.assertIn("__system_property_get", requested)
        self.assertTrue(start.lstrip().startswith("if (!AndroidRawPcmCaptureRequested())"))
        self.assertLess(
            start.index("AndroidRawPcmCaptureRequested"),
            start.index("std::make_unique<AndroidRawPcmCapture>"),
        )

    def test_guest_capture_happens_before_backend_conversion(self) -> None:
        output = method_body(BACKEND, "void Output(void* ptr)")
        self.assertLess(
            output.index("raw_pcm_capture->AppendGuest(ptr, buffer_frames)"),
            output.index("ConvertToOutput(ptr)"),
        )
        self.assertLess(
            output.index("raw_pcm_capture->AppendGuest(ptr, buffer_frames)"),
            output.index("CanSubmitOutput()"),
        )
        self.assertIn("if (raw_pcm_capture) [[unlikely]]", output)

    def test_normalized_float_entry_is_captured_before_submission(self) -> None:
        output = method_body(BACKEND, "void OutputFloat")
        self.assertLess(
            output.index("raw_pcm_capture->AppendFloat(ptr, frames, channels)"),
            output.index("CanSubmitOutput()"),
        )

    def test_capture_is_preallocated_and_strictly_bounded(self) -> None:
        supports = method_body(RAW_CAPTURE, "static bool Supports")
        append = method_body(RAW_CAPTURE, "void AppendGuest")
        self.assertIn("static constexpr u32 CaptureSeconds = 12", RAW_CAPTURE)
        self.assertIn("static constexpr u64 MaxCaptureBytes", RAW_CAPTURE)
        self.assertIn("output_bytes <= MaxCaptureBytes", supports)
        self.assertIn("samples(static_cast<size_t>(target_frames) * channels)", RAW_CAPTURE)
        self.assertIn("target_frames - captured_frames", append)
        self.assertNotIn(".resize(", append)
        self.assertNotIn("new ", append)

    def test_producer_only_copies_or_sample_converts_without_file_io(self) -> None:
        append_guest = method_body(RAW_CAPTURE, "void AppendGuest")
        append_float = method_body(RAW_CAPTURE, "void AppendFloat")
        producer = append_guest + append_float
        self.assertIn("std::memcpy", producer)
        self.assertIn("INV_VOLUME_0DB", append_guest)
        for forbidden in (
            "fopen",
            "fwrite",
            "fclose",
            "mutex",
            "lock_guard",
            "scoped_lock",
            "sleep",
            "AndroidAudioLog",
        ):
            self.assertNotIn(forbidden, producer)

    def test_file_io_is_isolated_to_the_writer_thread(self) -> None:
        writer = method_body(RAW_CAPTURE, "void WriteCapture")
        callback = method_body(BACKEND, "aaudio_data_callback_result_t OnAAudioData")
        error_callback = method_body(BACKEND, "static void AAudioErrorCallback")
        self.assertIn("writer_signal.acquire()", RAW_CAPTURE)
        self.assertIn("WriteCapture()", RAW_CAPTURE)
        self.assertIn("std::fopen", writer)
        self.assertIn("std::fwrite", writer)
        self.assertNotIn("raw_pcm_capture", callback + error_callback)
        self.assertNotIn("fwrite", callback + error_callback)

    def test_output_is_float32_wav_at_a_deterministic_internal_path(self) -> None:
        writer = method_body(RAW_CAPTURE, "void WriteCapture")
        self.assertIn('"diag-audio-raw-preconvert-f32.wav"', SOURCE)
        self.assertIn("Common::FS::PathType::UserDir", RAW_CAPTURE)
        self.assertIn("std::remove(output_path.c_str())", RAW_CAPTURE)
        self.assertIn('std::memcpy(header.data(), "RIFF", 4)', writer)
        self.assertIn("StoreU16(header, 20, 3)", writer)
        self.assertIn("StoreU16(header, 34, 32)", writer)

    def test_only_one_port_can_own_the_bounded_process_capture(self) -> None:
        start = method_body(BACKEND, "void StartRawPcmCapture")
        destructor = method_body(RAW_CAPTURE, "~AndroidRawPcmCapture()")
        self.assertIn("AndroidRawPcmCaptureOwner.test_and_set", start)
        self.assertIn("AndroidRawPcmCaptureOwner.clear", destructor)


if __name__ == "__main__":
    unittest.main()
