import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
AUDIOOUT = (
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


class AudioMultiPortWaitContractTests(unittest.TestCase):
    def test_wait_releases_every_unrelated_port_lock(self):
        outputs = function_body(
            AUDIOOUT,
            "s32 PS4_SYSV_ABI sceAudioOutOutputs",
            "s32 PS4_SYSV_ABI sceAudioOutSetVolume",
        )

        acquire_all = outputs.index("for (const auto& port : lock_order)")
        select_waiter = outputs.index("std::size_t wait_index", acquire_all)
        unlock_others = outputs.index("if (index != wait_index)", select_waiter)
        wait = outputs.index("wait_port->output_cv.wait", unlock_others)
        revalidate = outputs.index("locks.clear();", wait)

        self.assertLess(acquire_all, select_waiter)
        self.assertLess(select_waiter, unlock_others)
        self.assertLess(unlock_others, wait)
        self.assertLess(wait, revalidate)
        self.assertIn("locks[index].unlock();", outputs[unlock_others:wait])
        retry_loop = outputs.rfind("for (;;)", 0, acquire_all)
        self.assertNotEqual(retry_loop, -1)
        self.assertLess(retry_loop, acquire_all)

    def test_hot_path_stays_sorted_blocking_and_poll_free(self):
        outputs = function_body(
            AUDIOOUT,
            "s32 PS4_SYSV_ABI sceAudioOutOutputs",
            "s32 PS4_SYSV_ABI sceAudioOutSetVolume",
        )
        lock_section = outputs.split("const u64 wait_started_ns", 1)[1]
        lock_section = lock_section.split("u32 ready_count", 1)[0]

        sort = lock_section.index("std::ranges::sort(lock_order")
        acquire = lock_section.index("locks.emplace_back(port->mutex)")
        ready = lock_section.index("if (wait_index == lock_order.size())")
        blocking_wait = lock_section.index("wait_port->output_cv.wait")

        self.assertLess(sort, acquire)
        self.assertLess(acquire, ready)
        self.assertLess(ready, blocking_wait)
        self.assertNotIn("try_lock", lock_section)
        self.assertNotIn("sleep", lock_section)
        self.assertNotIn("yield", lock_section)


if __name__ == "__main__":
    unittest.main()
