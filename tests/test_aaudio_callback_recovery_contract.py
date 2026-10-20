from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (
    ROOT / "funnel-arm/src/core/libraries/audio/sdl_audio_out.cpp"
).read_text(encoding="utf-8")
BACKEND = SOURCE[SOURCE.index("class AndroidAAudioPortBackend") :]


def method_body(signature: str) -> str:
    start = BACKEND.index(signature)
    opening = BACKEND.index("{", start)
    depth = 0
    for index in range(opening, len(BACKEND)):
        char = BACKEND[index]
        if char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return BACKEND[opening + 1 : index]
    raise AssertionError(f"unterminated method: {signature}")


class AAudioCallbackRecoveryContract(unittest.TestCase):
    def test_builder_registers_error_callback_only_with_data_callback(self) -> None:
        open_stream = method_body("bool OpenStream()")
        callback_branch = open_stream[open_stream.index("if (callback_transport)") :]
        self.assertIn(
            "AAudioStreamBuilder_setDataCallback(builder, AAudioDataCallback, this)",
            callback_branch,
        )
        self.assertIn(
            "AAudioStreamBuilder_setErrorCallback(builder, AAudioErrorCallback, this)",
            callback_branch,
        )
        before_branch = open_stream[: open_stream.index("if (callback_transport)")]
        self.assertNotIn("AAudioStreamBuilder_setErrorCallback", before_branch)

    def test_error_callback_is_rt_safe_and_only_signals_control_thread(self) -> None:
        callback = method_body("static void AAudioErrorCallback")
        signal = method_body("void SignalRecovery")
        self.assertIn("SignalRecovery(failed_stream, error)", callback)
        self.assertIn("recovery_pending.exchange", signal)
        self.assertIn("recovery_signal.release()", signal)
        for forbidden in (
            "AAudioStream_request",
            "AAudioStream_close",
            "OpenStream",
            "RecoverStream",
            "AndroidAudioLog",
            "LOG_",
            "mutex",
            "lock_guard",
            "scoped_lock",
            "unique_lock",
            "sleep",
            "new ",
            ".resize(",
        ):
            self.assertNotIn(forbidden, callback + signal)

    def test_lifecycle_work_is_owned_by_the_recovery_thread(self) -> None:
        loop = method_body("void RecoveryLoop")
        recover = method_body("bool RecoverStream")
        close = method_body("void CloseOwnedStream")
        self.assertIn("std::jthread", BACKEND)
        self.assertIn("recovery_signal.try_acquire_for", loop)
        self.assertIn("RecoverStream", loop)
        self.assertIn("CloseOwnedStream", recover)
        self.assertIn("OpenStream", recover)
        self.assertIn("AAudioStream_requestStop", close)
        self.assertIn("AAudioStream_close", close)

    def test_disconnect_unpublishes_and_discards_stale_fifo_before_reopen(self) -> None:
        recover = method_body("bool RecoverStream")
        reset = method_body("void ResetCallbackQueueForRestart")
        self.assertLess(
            recover.index("published_stream.store(nullptr"),
            recover.index("CloseOwnedStream"),
        )
        self.assertLess(recover.index("CloseOwnedStream"), recover.index("OpenStream"))
        self.assertIn("callback_write_frame.load", reset)
        self.assertIn("callback_read_frame.store(write_frame", reset)
        self.assertIn(
            "callback_primed.store(false, std::memory_order_relaxed)", reset
        )

    def test_recovery_and_data_callback_share_primed_state_atomically(self) -> None:
        callback = method_body("aaudio_data_callback_result_t OnAAudioData")
        reset = method_body("void ResetCallbackQueueForRestart")
        self.assertIn("std::atomic<bool> callback_primed", BACKEND)
        self.assertIn("callback_primed.load(std::memory_order_relaxed)", callback)
        self.assertIn("callback_primed.store(true, std::memory_order_relaxed)", callback)
        self.assertIn("callback_primed.store(false, std::memory_order_relaxed)", callback)
        self.assertIn("callback_primed.store(false, std::memory_order_relaxed)", reset)

    def test_watchdog_is_heartbeat_driven_and_retries_with_bounded_backoff(self) -> None:
        loop = method_body("void RecoveryLoop")
        self.assertIn("callback_consumer_heartbeat", loop)
        self.assertIn("callback_producer_heartbeat", loop)
        self.assertIn("CallbackQueueHasFrames()", loop)
        self.assertIn("constexpr u32 StallPollLimit = 3", loop)
        for delay in ("100", "250", "500", "1000", "2000", "5000"):
            self.assertIn(f"milliseconds{{{delay}}}", loop)

    def test_shutdown_joins_before_the_owned_stream_is_closed(self) -> None:
        cleanup = method_body("void Cleanup")
        stop = method_body("void StopRecoveryThread")
        self.assertLess(cleanup.index("StopRecoveryThread"), cleanup.index("CloseOwnedStream"))
        self.assertIn("recovery_shutting_down.store(true", stop)
        self.assertIn("published_stream.store(nullptr", stop)
        self.assertLess(stop.index("request_stop"), stop.index("join()"))

    def test_legacy_synchronous_write_path_remains_present(self) -> None:
        submit = method_body("void SubmitOutputBuffer")
        callback_return = submit.index("(void)EnqueueCallbackBuffer(probe_id)")
        legacy_write = submit.index("AAudioStream_write(")
        self.assertLess(callback_return, legacy_write)
        self.assertIn("while (total_written < static_cast<int32_t>(buffer_frames))", submit)


if __name__ == "__main__":
    unittest.main()
