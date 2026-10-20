from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (
    ROOT / "funnel-arm/src/core/libraries/audio/sdl_audio_out.cpp"
).read_text(encoding="utf-8")
PORT = (
    ROOT / "funnel-arm/src/core/libraries/audio/audioout.h"
).read_text(encoding="utf-8")


class AAudioCallbackTransportContract(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        backend = SOURCE.index("class AndroidAAudioPortBackend")
        cls.backend = SOURCE[backend:]
        callback = cls.backend.index("aaudio_data_callback_result_t OnAAudioData")
        submit = cls.backend.index("void SubmitOutputBuffer", callback)
        cls.callback_body = cls.backend[callback:submit]

    def test_callback_transport_is_explicit_and_legacy_remains_default(self) -> None:
        self.assertIn('"debug.lsx4.audio_transport"', SOURCE)
        self.assertIn('std::strcmp(value, "callback") == 0', SOURCE)
        self.assertIn("bool callback_transport{}", self.backend)
        self.assertIn("if (callback_transport)", self.backend)
        self.assertIn("AAudioStream_write(", self.backend)

    def test_consumed_credit_is_explicit_and_scoped_to_callback_ports(self) -> None:
        self.assertIn('"debug.lsx4.audio_credit"', SOURCE)
        self.assertIn('std::strcmp(value, "consume") == 0', SOURCE)
        self.assertIn(
            "port.strict_output_credit = AndroidAudioCallbackTransportRequested() &&",
            SOURCE,
        )
        self.assertIn("AndroidAudioConsumedCreditRequested()", SOURCE)
        self.assertIn(
            "ready_slot < 0 && (!strict_output_credit || processing_slot < 0)",
            PORT,
        )

    def test_fifo_is_preallocated_for_eight_blocks_and_primes_two(self) -> None:
        self.assertIn("callback_fifo_capacity_frames = buffer_frames * 8", self.backend)
        self.assertIn("callback_prime_frames = buffer_frames * 2", self.backend)
        self.assertIn("callback_fifo.resize", self.backend)
        self.assertIn("callback_write_frame.store", self.backend)
        self.assertIn("callback_read_frame.store", self.callback_body)
        self.assertIn("std::max<u64>(callback_prime_frames", self.callback_body)

    def test_callback_is_registered_only_for_the_callback_transport(self) -> None:
        registration = "AAudioStreamBuilder_setDataCallback(builder, AAudioDataCallback, this)"
        self.assertIn(registration, self.backend)
        open_stream = self.backend.index("bool OpenStream()")
        registration_at = self.backend.index(registration, open_stream)
        branch_at = self.backend.rfind("if (callback_transport)", open_stream, registration_at)
        self.assertGreaterEqual(branch_at, open_stream)

    def test_realtime_callback_has_no_logging_locking_or_allocation(self) -> None:
        for forbidden in (
            "AndroidAudioLog",
            "AndroidAudioProbeWarn",
            "LOG_",
            "std::mutex",
            "scoped_lock",
            "unique_lock",
            ".resize(",
            "new ",
        ):
            self.assertNotIn(forbidden, self.callback_body)
        self.assertIn("std::memcpy", self.callback_body)
        self.assertIn("std::memset", self.callback_body)

    def test_overflow_drops_newest_without_advancing_the_consumer(self) -> None:
        enqueue = self.backend.index("bool EnqueueCallbackBuffer")
        callback = self.backend.index("static aaudio_data_callback_result_t", enqueue)
        enqueue_body = self.backend[enqueue:callback]
        overflow = enqueue_body.index(
            "queued_frames > callback_fifo_capacity_frames - buffer_frames"
        )
        returned = enqueue_body.index("return false", overflow)
        publish = enqueue_body.index("callback_write_frame.store", overflow)
        self.assertLess(returned, publish)
        overflow_branch = enqueue_body[overflow:returned]
        self.assertNotIn("callback_read_frame.store", overflow_branch)


if __name__ == "__main__":
    unittest.main()
