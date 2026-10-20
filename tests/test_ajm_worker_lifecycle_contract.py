from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "funnel-arm/src/core/libraries/ajm/ajm_context.h"
SOURCE = ROOT / "funnel-arm/src/core/libraries/ajm/ajm_context.cpp"


class AjmWorkerLifecycleContractTests(unittest.TestCase):
    def test_queue_outlives_worker(self) -> None:
        text = HEADER.read_text(encoding="utf-8")
        self.assertLess(text.index("batch_queue;"), text.index("worker_thread{};"))

    def test_shutdown_wakes_worker_before_join(self) -> None:
        text = SOURCE.read_text(encoding="utf-8")
        destructor = text[text.index("AjmContext::~AjmContext()") :]
        stop = destructor.index("worker_thread.request_stop();")
        wake = destructor.index("batch_queue.TryEmplace")
        join = destructor.index("worker_thread.join();")
        self.assertLess(stop, wake)
        self.assertLess(wake, join)


if __name__ == "__main__":
    unittest.main()
