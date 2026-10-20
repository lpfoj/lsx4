from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
PTHREAD = ROOT / "funnel-arm/src/core/libraries/kernel/threads/pthread.h"
CONDVAR = ROOT / "funnel-arm/src/core/libraries/kernel/threads/condvar.cpp"


class PthreadWakeTokenContractTests(unittest.TestCase):
    def test_condvar_wakes_share_the_tracked_token_path(self):
        header = PTHREAD.read_text(encoding="utf-8")
        condvar = CONDVAR.read_text(encoding="utf-8")

        self.assertIn("Pthread* defer_waiters[MaxDeferWaiters]", header)
        self.assertIn('defer_waiters[i]->PostWake("cond-deferred")', header)
        self.assertIn('wake_thread->PostWake("cond-signal")', condvar)
        self.assertIn('waiters[i]->PostWake("cond-broadcast")', condvar)
        self.assertNotIn("waddr->release()", condvar)
        self.assertNotIn("waddrs[i]->release()", condvar)


if __name__ == "__main__":
    unittest.main()
