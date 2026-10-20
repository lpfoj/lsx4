import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PM4_DIFF_TRACE = (
    ROOT
    / "funnel-arm"
    / "src"
    / "video_core"
    / "amdgpu"
    / "pm4_diff_trace.h"
).read_text(encoding="utf-8")
LIVERPOOL = (
    ROOT
    / "funnel-arm"
    / "src"
    / "video_core"
    / "amdgpu"
    / "liverpool.cpp"
).read_text(encoding="utf-8")


def cpp_function(source: str, signature: str) -> str:
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 0
    for position in range(brace, len(source)):
        token = source[position]
        if token == "{":
            depth += 1
        elif token == "}":
            depth -= 1
            if depth == 0:
                return source[start : position + 1]
    raise AssertionError(f"unterminated function: {signature}")


class Pm4ResourceWriteWatchPollingContractTests(unittest.TestCase):
    def test_draw_probe_never_touches_the_marker_file(self):
        probe = cpp_function(
            PM4_DIFF_TRACE, "inline void ProbeRequestedResourceWriteWatch"
        )

        for forbidden in ("fopen", "fscanf", "fclose", "std::remove"):
            self.assertNotIn(forbidden, probe)
        self.assertIn("resource_write_watch_request.load", probe)
        self.assertIn("std::memory_order_acquire", probe)
        self.assertIn("ResourceWriteWatchRequestValid", probe)

    def test_poll_publishes_only_a_complete_valid_request(self):
        poll = cpp_function(
            PM4_DIFF_TRACE, "inline void PollRequestedResourceWriteWatchMarker"
        )

        self.assertIn("std::fopen(ResourceWriteWatchMarker", poll)
        self.assertIn("std::fscanf", poll)
        self.assertIn("fields != 4", poll)
        self.assertIn("requested_bytes > 4096", poll)
        self.assertIn(
            "resource_write_watch_request.store(0, std::memory_order_release)",
            " ".join(poll.split()),
        )
        self.assertIn("PackResourceWriteWatchRequest", poll)
        self.assertNotIn("std::remove", poll)

    def test_request_is_packed_into_one_atomic_publication(self):
        pack = cpp_function(
            PM4_DIFF_TRACE, "inline u64 PackResourceWriteWatchRequest"
        )

        self.assertIn("ResourceWriteWatchRequestValid", pack)
        self.assertIn("ResourceWriteWatchStageShift", pack)
        self.assertIn("ResourceWriteWatchPairShift", pack)
        self.assertIn("ResourceWriteWatchSlotShift", pack)
        self.assertIn("ResourceWriteWatchBytesShift", pack)
        self.assertIn(
            "std::atomic<u64> resource_write_watch_request{0}", PM4_DIFF_TRACE
        )

    def test_successful_arm_is_an_exact_one_shot(self):
        arm = cpp_function(PM4_DIFF_TRACE, "inline bool TryArmResourceWriteWatch")

        self.assertIn("resource_write_watch_claimed.compare_exchange_strong", arm)
        self.assertIn("resource_write_watch_request.compare_exchange_strong", arm)
        self.assertLess(
            arm.index("resource_write_watch_request.compare_exchange_strong"),
            arm.index("executor_funnel_arm_diagnostic_write_provenance"),
        )
        failure = arm.split(
            "if (!executor_funnel_arm_diagnostic_write_provenance", 1
        )[1].split("(void)std::remove", 1)[0]
        self.assertIn("resource_write_watch_claimed.store(false", failure)
        self.assertIn("empty_request, requested", failure)
        success = arm.split("(void)std::remove", 1)[1]
        self.assertNotIn("resource_write_watch_claimed.store(false", success)

    def test_marker_is_polled_at_startup_and_once_per_patched_flip(self):
        constructor = cpp_function(LIVERPOOL, "Liverpool::Liverpool()")
        self.assertEqual(
            constructor.count("PollRequestedResourceWriteWatchMarker()"), 1
        )

        flip = LIVERPOOL.split(
            "case PM4CmdNop::PayloadType::PatchedFlip:", 1
        )[1].split("case PM4CmdNop::PayloadType::DebugMarkerPush:", 1)[0]
        self.assertEqual(flip.count("PollRequestedResourceWriteWatchMarker()"), 1)
        self.assertEqual(LIVERPOOL.count("PollRequestedResourceWriteWatchMarker()"), 2)


if __name__ == "__main__":
    unittest.main()
