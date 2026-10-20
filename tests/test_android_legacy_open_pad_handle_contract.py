from pathlib import Path
import unittest


PAD_CPP = (
    Path(__file__).resolve().parents[1]
    / "funnel-arm"
    / "src"
    / "core"
    / "libraries"
    / "pad"
    / "pad.cpp"
)


def _sce_pad_read_body() -> str:
    source = PAD_CPP.read_text(encoding="utf-8")
    start = source.index("int PS4_SYSV_ABI scePadRead(")
    end = source.index("int PS4_SYSV_ABI scePadReadBlasterForTracker", start)
    return source[start:end]


class LegacyOpenPadHandleContractTests(unittest.TestCase):
    def test_legacy_invalid_handle_is_normalized_only_after_pad_open(self) -> None:
        body = _sce_pad_read_body()
        guarded_normalization = (
            "handle == Libraries::UserService::ORBIS_USER_SERVICE_USER_ID_INVALID && g_opened"
        )

        self.assertIn(guarded_normalization, body)
        self.assertLess(
            body.index(guarded_normalization), body.index("if (handle < 1 || !pData)")
        )

    def test_legacy_open_handle_uses_real_controller_samples(self) -> None:
        body = _sce_pad_read_body()
        legacy_guard = body.index(
            "handle == Libraries::UserService::ORBIS_USER_SERVICE_USER_ID_INVALID && g_opened"
        )
        dummy_declaration = body.index("bool executor_dummy_handle = false;")
        real_read = body.index("controller->ReadStates(")

        self.assertLess(legacy_guard, dummy_declaration)
        self.assertLess(dummy_declaration, real_read)
        self.assertIn("handle = 1;", body[legacy_guard:dummy_declaration])
        self.assertNotIn("executor_dummy_handle = true", body[legacy_guard:dummy_declaration])

    def test_existing_error_handle_path_remains_disconnected_dummy(self) -> None:
        body = _sce_pad_read_body()

        self.assertIn("handle == ORBIS_PAD_ERROR_NOT_INITIALIZED", body)
        self.assertIn("handle == ORBIS_PAD_ERROR_DEVICE_NO_HANDLE", body)
        self.assertIn("executor_dummy_handle = true;", body)
        self.assertIn("dummy normalized handle: returning zero samples", body)


if __name__ == "__main__":
    unittest.main()
