from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
AUDIO = (
    ROOT / "funnel-arm/src/core/libraries/audio/audioout.cpp"
).read_text(encoding="utf-8")
AAUDIO = (
    ROOT / "funnel-arm/src/core/libraries/audio/sdl_audio_out.cpp"
).read_text(encoding="utf-8")
AJM = (
    ROOT / "funnel-arm/src/core/libraries/ajm/ajm.cpp"
).read_text(encoding="utf-8")
RUNTIME = (ROOT / "src/executor/native_runtime_api.cpp").read_text(encoding="utf-8")


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


class AudioTitleSessionLifecycleContract(unittest.TestCase):
    def test_pause_preserves_ports_and_only_gates_scheduler_consumption(self) -> None:
        setter = function_body(AUDIO, "void SetSessionPaused")
        worker = function_body(AUDIO, "void WorkerLoop")
        self.assertIn("session_paused.store(true", setter)
        self.assertIn("SetHostStreamsPaused(true)", setter)
        self.assertIn("SetHostStreamsPaused(false)", setter)
        self.assertLess(setter.index("SetHostStreamsPaused(false)"),
                        setter.index("session_paused.store(false"))
        self.assertIn("NotifyLifecycleChange", setter)
        self.assertIn("!session_paused.load", worker)
        for destructive in ("port_table", "impl.reset", "audio.reset", "SDL_Quit"):
            self.assertNotIn(destructive, setter)

    def test_full_reset_wakes_producers_joins_scheduler_then_closes_backends(self) -> None:
        reset = function_body(AUDIO, "void ResetForTitleSession")
        closing = reset.index("port->closing = true")
        wake = reset.index("port->output_cv.notify_all()")
        join = reset.index("AudioScheduler().StopAndReset()")
        unpause = reset.index("session_paused.store(false", join)
        table_clear = reset.index("port.reset()", join)
        backend_close = reset.index("port->impl.reset()", table_clear)
        backend_owner = reset.index("audio.reset()", backend_close)
        lazy = reset.index("lazy_init.store(0", backend_owner)
        self.assertLess(closing, wake)
        self.assertLess(wake, join)
        self.assertLess(join, unpause)
        self.assertLess(unpause, table_clear)
        self.assertLess(join, table_clear)
        self.assertLess(table_clear, backend_close)
        self.assertLess(backend_close, backend_owner)
        self.assertLess(backend_owner, lazy)

    def test_aaudio_close_unpublishes_and_waits_before_release(self) -> None:
        close = function_body(AAUDIO, "void CloseOwnedStream")
        unpublish = close.index("published_stream.store(nullptr")
        stop = close.index("AAudioStream_requestStop")
        wait = close.index("AAudioStream_waitForStateChange")
        release = close.index("AAudioStream_close")
        self.assertLess(unpublish, stop)
        self.assertLess(stop, wait)
        self.assertLess(wait, release)
        self.assertIn("StopWaitSliceNs = INT64_C(125000000)", close)
        self.assertIn("StopWaitTransitions = 4", close)
        self.assertIn("AAUDIO_STREAM_STATE_STOPPED", close)
        self.assertIn("AAUDIO_STREAM_STATE_DISCONNECTED", close)

    def test_disable_audio_escape_is_snapshotted_per_backend_not_per_process(self) -> None:
        request = function_body(AAUDIO, "static bool AndroidAudioOutputDisabledRequested")
        disabled = function_body(AAUDIO, "bool AudioOutputDisabled() const noexcept")
        selector = function_body(AAUDIO, "std::unique_ptr<PortBackend> SDLAudioOut::Open")
        self.assertIn('std::getenv("EXECUTOR_DISABLE_AUDIO_OUTPUT")', request)
        self.assertEqual("return audio_output_disabled;", disabled.strip())
        self.assertIn(
            "const bool audio_out_disabled = AndroidAudioOutputDisabledRequested()",
            selector,
        )
        self.assertNotIn("static const bool audio_out_disabled", selector)

        initialize = function_body(
            RUNTIME, 'extern "C" int executor_lsx4_runtime_initialize'
        )
        sentinel = initialize.index('"run-disable-audio-output"')
        disable_policy = initialize[sentinel : sentinel + 1000]
        self.assertIn('setenv("EXECUTOR_DISABLE_AUDIO_OUTPUT", "1", 1)', disable_policy)
        self.assertIn('unsetenv("EXECUTOR_DISABLE_AUDIO_OUTPUT")', disable_policy)

    def test_ajm_reset_destroys_context_workers_outside_global_map(self) -> None:
        reset = function_body(AJM, "void ResetForTitleSession")
        finalize = function_body(AJM, "int PS4_SYSV_ABI sceAjmFinalize")
        self.assertIn("auto retired_contexts = std::move(contexts)", reset)
        self.assertLess(reset.index("contexts.clear()"), reset.index("retired_contexts.clear()"))
        self.assertIn("ResetForTitleSession()", finalize)
        self.assertNotIn("STUBBED", finalize)

    def test_every_synchronous_launch_exit_runs_full_reset(self) -> None:
        launch = function_body(
            RUNTIME, 'extern "C" int executor_lsx4_runtime_launch_game_jit'
        )
        export = function_body(
            RUNTIME, 'extern "C" int executor_lsx4_runtime_reset_title_session'
        )
        self.assertIn("struct SessionResetGuard", launch)
        self.assertGreaterEqual(
            launch.count("executor_lsx4_runtime_reset_title_session()"), 2
        )
        ps1_branch = launch.index("if (Executor::Ps1Classic::IsPackage")
        ps1_launch = launch.index("return Executor::Ps1Classic::Launch", ps1_branch)
        guard = launch.index("struct SessionResetGuard")
        self.assertLess(ps1_launch, guard)
        self.assertIn("Libraries::AudioOut::ResetForTitleSession()", export)
        self.assertIn("Libraries::Ajm::ResetForTitleSession()", export)
        self.assertLess(
            export.index("Libraries::AudioOut::ResetForTitleSession()"),
            export.index("Libraries::Ajm::ResetForTitleSession()"),
        )

    def test_title_b_initialization_cannot_inherit_title_a_audio_state(self) -> None:
        initialize = function_body(
            RUNTIME, 'extern "C" int executor_lsx4_runtime_initialize'
        )
        initialize_audio = initialize.index("Libraries::AudioOut::ResetForTitleSession()")
        initialize_ajm = initialize.index("Libraries::Ajm::ResetForTitleSession()")
        publish_root = initialize.index("g_root = root_dir")
        self.assertLess(initialize_audio, initialize_ajm)
        self.assertLess(initialize_ajm, publish_root)

        launch = function_body(
            RUNTIME, 'extern "C" int executor_lsx4_runtime_launch_game_jit'
        )
        self.assertLess(
            launch.index("executor_lsx4_runtime_reset_title_session()"),
            launch.index("Executor::Ps1Classic::IsPackage"),
        )


if __name__ == "__main__":
    unittest.main()
