import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
EVENT_FLAG = (
    ROOT
    / "funnel-arm"
    / "src"
    / "core"
    / "libraries"
    / "kernel"
    / "threads"
    / "event_flag.cpp"
).read_text(encoding="utf-8")
SEMAPHORE = (
    ROOT
    / "funnel-arm"
    / "src"
    / "core"
    / "libraries"
    / "kernel"
    / "threads"
    / "semaphore.cpp"
).read_text(encoding="utf-8")


def section(source: str, start: str, end: str) -> str:
    return source.split(start, 1)[1].split(end, 1)[0]


class MonoEventFlagSafepointContractTests(unittest.TestCase):
    def test_targeted_pending_signal_wakes_event_flag_waiter(self):
        waiter = section(
            EVENT_FLAG,
            "class EventFlagPendingSignalWaiter",
            "constexpr int ORBIS_KERNEL_EVF_ATTR_TH_FIFO",
        )
        self.assertIn("waiter->guest_thread != target", waiter)
        self.assertIn(
            "signal_pending.store(true, std::memory_order_release)", waiter
        )
        self.assertIn("waiter->condition->notify_all()", waiter)
        self.assertIn("std::atomic_bool signal_pending", waiter)

        dispatcher = section(
            SEMAPHORE,
            'extern "C" void executor_live_mono_notify_pending_signal_waiter',
            "#endif",
        )
        self.assertIn(
            "executor_live_mono_notify_pending_event_flag_waiter(target)",
            dispatcher,
        )

    def test_normal_event_flags_use_notifications_not_periodic_polling(self):
        wait = section(EVENT_FLAG, "int Wait(u64 bits", "int Poll(u64 bits")
        ordinary = section(
            wait,
            "if (!needs_mono_safe_point_poll)",
            "} else if (infinitely)",
        )
        self.assertIn("pending_signal_waiter.Pending()", ordinary)
        self.assertIn("pending_signal_waiter.Consume()", ordinary)
        self.assertIn("RunMonoPendingSignalSafePointFromEventFlagWait(lock)", ordinary)
        self.assertNotIn("milliseconds(1)", ordinary)
        self.assertNotIn("sleep_for", ordinary)

    def test_waiter_lifetime_is_registered_without_heap_allocation(self):
        wait = section(EVENT_FLAG, "int Wait(u64 bits", "int Poll(u64 bits")
        self.assertIn(
            "EventFlagPendingSignalWaiter pending_signal_waiter{g_curthread, m_cond_var}",
            wait,
        )
        registry = section(
            EVENT_FLAG,
            "class EventFlagPendingSignalWaiter",
            "constexpr int ORBIS_KERNEL_EVF_ATTR_TH_FIFO",
        )
        self.assertIn("RegistryHead() = this", registry)
        self.assertIn("*link = next", registry)
        self.assertNotIn("new EventFlagPendingSignalWaiter", registry)


if __name__ == "__main__":
    unittest.main()
