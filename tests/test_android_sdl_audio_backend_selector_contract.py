from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "funnel-arm/src/core/libraries/audio/sdl_audio_out.cpp"


class AndroidSdlAudioBackendSelectorContract(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.source = SOURCE.read_text(encoding="utf-8")
        open_start = cls.source.index("std::unique_ptr<PortBackend> SDLAudioOut::Open")
        cls.open_body = cls.source[open_start:]

    def test_selector_accepts_property_or_explicit_environment_override(self) -> None:
        self.assertIn('"debug.lsx4.audio_backend"', self.source)
        self.assertIn('std::strcmp(value, "sdl") == 0', self.source)
        self.assertIn('std::getenv("EXECUTOR_FORCE_SDL_AUDIO")', self.source)
        self.assertIn('std::strcmp(force_sdl, "1") == 0', self.source)

    def test_forced_sdl_returns_before_aaudio_is_constructed(self) -> None:
        selector = self.open_body.index("if (AndroidSdlAudioRequested())")
        direct_sdl = self.open_body.index("return std::make_unique<SDLPortBackend>(port);", selector)
        aaudio = self.open_body.index("std::make_unique<AndroidAAudioPortBackend>(port)")
        self.assertLess(selector, direct_sdl)
        self.assertLess(direct_sdl, aaudio)

    def test_selector_logs_effective_backend_once(self) -> None:
        selector = self.open_body.index("if (AndroidSdlAudioRequested())")
        aaudio = self.open_body.index("std::make_unique<AndroidAAudioPortBackend>(port)")
        forced_branch = self.open_body[selector:aaudio]
        self.assertIn("static std::atomic_flag logged = ATOMIC_FLAG_INIT", forced_branch)
        self.assertIn("requested=sdl effective=sdl", forced_branch)


if __name__ == "__main__":
    unittest.main()
