import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (
    ROOT
    / "funnel-arm"
    / "src"
    / "core"
    / "libraries"
    / "audio"
    / "audioout.cpp"
).read_text(encoding="utf-8")


def function_body(source: str, signature: str, next_signature: str) -> str:
    return source.split(signature, 1)[1].split(next_signature, 1)[0]


class AudioSchedulerDeadlineContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.acquire = function_body(SOURCE, "void AcquireBatch", "static void PaceBatch")
        cls.complete = function_body(SOURCE, "static void CompleteBatch", "void WorkerLoop")

    def test_ready_timestamp_reaches_completion_as_submit_timestamp(self):
        self.assertIn(".submit_timestamp_ns = port->ready_timestamp_ns", self.acquire)

    def test_buffer_arriving_after_deadline_discards_empty_tick_debt(self):
        late = self.complete.index("item.submit_timestamp_ns > item.deadline_ns")
        reset = self.complete.index(
            "port.next_output_deadline_ns = scheduled_at_ns + period_ns"
        )

        self.assertLess(late, reset)
        self.assertIn("buffer_arrived_after_deadline", self.complete[late:reset])

    def test_ready_before_deadline_preserves_scheduled_cadence(self):
        self.assertIn(
            "port.next_output_deadline_ns = item.deadline_ns + period_ns",
            self.complete,
        )


if __name__ == "__main__":
    unittest.main()
