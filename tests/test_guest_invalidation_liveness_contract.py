import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
RASTERIZER_H = (
    ROOT
    / "funnel-arm"
    / "src"
    / "video_core"
    / "renderer_vulkan"
    / "vk_rasterizer.h"
).read_text(encoding="utf-8")
RASTERIZER_CPP = (
    ROOT
    / "funnel-arm"
    / "src"
    / "video_core"
    / "renderer_vulkan"
    / "vk_rasterizer.cpp"
).read_text(encoding="utf-8")


def guest_invalidation_entry() -> str:
    return RASTERIZER_CPP.split(
        "bool Rasterizer::InvalidateMemoryFromGuestThread(\n"
        "    VAddr addr, u64 size, GuestInvalidationValidator validator,",
        1,
    )[1].split("bool Rasterizer::InvalidateMemoryBatchOnGpuThread", 1)[0]


def guest_invalidation_drain() -> str:
    return RASTERIZER_CPP.split("void Rasterizer::DrainGuestInvalidations()", 1)[
        1
    ].split("bool Rasterizer::ReadMemory", 1)[0]


class GuestInvalidationLivenessContractTests(unittest.TestCase):
    def test_terminal_is_rejected_before_a_stack_request_is_published(self):
        entry = guest_invalidation_entry()
        terminal = "liverpool == nullptr || liverpool->IsRendererTerminal()"

        self.assertIn(terminal, entry)
        self.assertLess(entry.index(terminal), entry.index("GuestInvalidationRequest request"))
        self.assertLess(entry.index(terminal), entry.index("pending_guest_invalidations.push_back"))

    def test_terminal_completion_clears_latch_and_releases_all_waiters(self):
        self.assertIn(
            "bool FailPendingGuestInvalidationsIfTerminalLocked() noexcept;",
            RASTERIZER_H,
        )
        terminal = RASTERIZER_CPP.split(
            "bool Rasterizer::FailPendingGuestInvalidationsIfTerminalLocked() noexcept",
            1,
        )[1].split("bool Rasterizer::InvalidateMemoryFromGuestThread(\n", 1)[0]

        self.assertIn("for (GuestInvalidationRequest* request", terminal)
        self.assertIn("request->result = false;", terminal)
        self.assertIn("request->retry = false;", terminal)
        self.assertIn("request->complete = true;", terminal)
        self.assertIn("pending_guest_invalidations.clear();", terminal)
        self.assertIn("guest_invalidation_drain_queued = false;", terminal)
        self.assertIn("guest_invalidation_handoff_pending = false;", terminal)
        self.assertIn("guest_invalidation_completion_cv.notify_all();", terminal)

    def test_handoff_is_consumed_once_before_completion_or_validation(self):
        entry = guest_invalidation_entry()
        self.assertIn(
            "bool guest_invalidation_handoff_pending{};", RASTERIZER_H
        )

        helper_start = entry.index(
            "const auto queue_guest_invalidation_handoff"
        )
        helper_end = entry.index("if (gpu_thread_fault)", helper_start)
        helper = entry[helper_start:helper_end]
        flag_test = helper.index("if (!guest_invalidation_handoff_pending)")
        consume = helper.index("guest_invalidation_handoff_pending = false;")
        unlock = helper.index("lock.unlock();", consume)
        send = helper.index("liverpool->SendCommand<false>", unlock)
        relock = helper.index("lock.lock();", send)

        self.assertLess(flag_test, consume)
        self.assertLess(consume, unlock)
        self.assertLess(unlock, send)
        self.assertLess(send, relock)

        ordinary_start = entry.index("if (request.validator == nullptr)")
        ordinary_end = entry.index("            } else {", ordinary_start)
        ordinary = entry[ordinary_start:ordinary_end]
        ordinary_terminal = ordinary.index(
            "FailPendingGuestInvalidationsIfTerminalLocked()"
        )
        ordinary_handoff = ordinary.index(
            "if (queue_guest_invalidation_handoff())"
        )
        ordinary_complete = ordinary.index("if (request.complete)")
        ordinary_rescue = ordinary.index("const bool rescue_drain")
        self.assertLess(ordinary_terminal, ordinary_handoff)
        self.assertLess(ordinary_handoff, ordinary_complete)
        self.assertLess(ordinary_complete, ordinary_rescue)

        validated_start = entry.index("auto rescue_deadline", ordinary_end)
        validated_handoff = entry.index(
            "if (queue_guest_invalidation_handoff())", validated_start
        )
        validated_complete = entry.index(
            "if (request.complete)", validated_handoff
        )
        validator_call = entry.index(
            "const bool current = request.validator", validated_complete
        )
        validator_relock = entry.index("lock.lock();", validator_call)
        post_validation_terminal = entry.index(
            "FailPendingGuestInvalidationsIfTerminalLocked()", validator_relock
        )
        post_validation_handoff = entry.index(
            "if (queue_guest_invalidation_handoff())", post_validation_terminal
        )
        post_validation_complete = entry.index(
            "if (request.complete)", post_validation_handoff
        )
        stale_erase = entry.index(
            "pending_guest_invalidations.erase(it)", post_validation_complete
        )
        self.assertLess(validated_handoff, validated_complete)
        self.assertLess(validated_complete, validator_call)
        self.assertLess(validator_call, validator_relock)
        self.assertLess(validator_relock, post_validation_terminal)
        self.assertLess(post_validation_terminal, post_validation_handoff)
        self.assertLess(post_validation_handoff, post_validation_complete)
        self.assertLess(post_validation_complete, stale_erase)

    def test_watchdog_rescue_is_single_shot_and_only_for_a_pending_request(self):
        entry = guest_invalidation_entry()

        self.assertIn(
            "ExecutorGuestInvalidationDrainWatchdogPeriod", RASTERIZER_CPP
        )
        self.assertIn(
            "ExecutorGuestInvalidationMaxWatchdogRescues = 1", RASTERIZER_CPP
        )
        self.assertNotIn("guest_invalidation_completion_cv.wait(\n", entry)
        self.assertGreaterEqual(
            entry.count("guest_invalidation_completion_cv.wait_for("), 2
        )
        self.assertGreaterEqual(
            entry.count(
                "pending_guest_invalidations.front() == &request"
            ),
            2,
        )
        self.assertGreaterEqual(
            entry.count(
                "watchdog_rescues <\n"
                "                            ExecutorGuestInvalidationMaxWatchdogRescues"
            ),
            2,
        )




        first_rescue = entry.index("const bool rescue_drain")
        first_send = entry.index("liverpool->SendCommand<false>", first_rescue)
        self.assertLess(
            entry.index("pending_guest_invalidations.front() == &request", first_rescue),
            first_send,
        )
        self.assertLess(entry.index("lock.unlock();", first_rescue), first_send)

    def test_gpu_thread_fault_stays_inline_and_never_consumes_the_handoff(self):
        entry = guest_invalidation_entry()
        publish = entry.index("if (gpu_thread_fault)")
        inline_drain = entry.index("DrainGuestInvalidations();", publish)
        wait_lock = entry.index(
            "std::unique_lock lock{guest_invalidation_mutex};", inline_drain
        )
        inline_check = entry.index("if (gpu_thread_fault)", wait_lock)
        non_gpu_wait = entry.index("        } else {", inline_check)
        gpu_only = entry[inline_check:non_gpu_wait]

        self.assertLess(publish, inline_drain)
        self.assertLess(inline_drain, wait_lock)
        self.assertIn("ASSERT_MSG(request.complete", gpu_only)
        self.assertNotIn("queue_guest_invalidation_handoff()", gpu_only)
        self.assertNotIn("guest_invalidation_completion_cv.wait", gpu_only)

    def test_drain_fail_completes_terminal_batches_without_vulkan_work(self):
        drain = guest_invalidation_drain()
        terminal_precheck = drain.index(
            "FailPendingGuestInvalidationsIfTerminalLocked()"
        )
        swap = drain.index("batch.swap(pending_guest_invalidations)")
        process = drain.index("const auto process_batch")
        batch_terminal = drain.index(
            "liverpool == nullptr || liverpool->IsRendererTerminal()", process
        )
        invalidate = drain.index("InvalidateMemory(request->addr", process)

        self.assertLess(terminal_precheck, swap)
        self.assertLess(batch_terminal, invalidate)
        self.assertIn("request->result = false;", drain[batch_terminal:invalidate])
        self.assertIn("request->retry = false;", drain[batch_terminal:invalidate])
        self.assertIn("request->retry = !renderer_terminal;", drain)
        self.assertIn("guest_invalidation_completion_cv.notify_all();", drain)

    def test_late_duplicate_callback_has_an_idempotent_empty_path(self):
        drain = guest_invalidation_drain()
        swap = drain.index("batch.swap(pending_guest_invalidations)")
        clear_handoff = drain.index(
            "guest_invalidation_handoff_pending = false;", swap
        )
        empty = drain.index("if (batch.empty())", swap)
        clear_latch = drain.index(
            "guest_invalidation_drain_queued = false;", empty
        )
        leave = drain.index("return;", clear_latch)
        process = drain.index("const auto process_batch")

        self.assertLess(swap, empty)
        self.assertLess(swap, clear_handoff)
        self.assertLess(clear_handoff, empty)
        self.assertLess(empty, clear_latch)
        self.assertLess(clear_latch, leave)
        self.assertLess(leave, process)

    def test_default_late_arrivals_are_retained_for_fifo_handoff(self):
        drain = guest_invalidation_drain()
        guard = drain.index("const bool handoff_late_arrivals")
        wake_count = drain.index("const size_t wake_count", guard)
        handoff_if = drain.index("if (handoff_late_arrivals)", wake_count)
        fallback = drain.index("                } else {", handoff_if)
        singleton = drain.index("// A singleton drain", fallback)
        handoff = drain[handoff_if:fallback]
        old_fallback = drain[fallback:singleton]

        guard_body = drain[guard:wake_count]
        self.assertIn("!renderer_terminal && !bounded_catchup", guard_body)
        self.assertIn("!pending_guest_invalidations.empty()", guard_body)
        self.assertIn("guest_invalidation_handoff_pending = true;", handoff)
        self.assertNotIn("pending_guest_invalidations.clear();", handoff)
        self.assertNotIn("guest_invalidation_drain_queued = false;", handoff)
        self.assertNotIn("request->complete = true;", handoff)
        self.assertNotIn("process_batch", handoff)
        self.assertNotIn("batch.swap", handoff)
        self.assertNotIn("SendCommand", handoff)




        self.assertIn("request->retry = !renderer_terminal;", old_fallback)
        self.assertIn("request->complete = true;", old_fallback)
        self.assertIn("pending_guest_invalidations.clear();", old_fallback)
        self.assertIn("guest_invalidation_drain_queued = false;", old_fallback)
        self.assertIn("guest_invalidation_handoff_pending = false;", old_fallback)

        wake_body = drain[wake_count:handoff_if]
        self.assertIn("completed_requests.size()", wake_body)
        self.assertIn("pending_guest_invalidations.size()", wake_body)

    def test_singleton_completion_is_targeted_under_the_publication_lock(self):
        drain = guest_invalidation_drain()
        final_state = drain.index("const size_t wake_count")
        pending_clear = drain.index(
            "pending_guest_invalidations.clear();", final_state
        )
        singleton = drain.index("if (wake_count == 1)", pending_clear)
        notify_one = drain.index(
            "guest_invalidation_completion_cv.notify_one();", singleton
        )
        lock_scope_end = drain.index("\n            }\n        }", notify_one)
        broadcast_guard = drain.index("if (wake_multiple_requests)", lock_scope_end)
        notify_all = drain.index(
            "guest_invalidation_completion_cv.notify_all();", broadcast_guard
        )

        self.assertLess(final_state, pending_clear)
        self.assertLess(pending_clear, singleton)
        self.assertLess(singleton, notify_one)
        self.assertLess(notify_one, lock_scope_end)
        self.assertLess(lock_scope_end, broadcast_guard)
        self.assertLess(broadcast_guard, notify_all)

        terminal = RASTERIZER_CPP.split(
            "bool Rasterizer::FailPendingGuestInvalidationsIfTerminalLocked() noexcept",
            1,
        )[1].split("bool Rasterizer::InvalidateMemoryFromGuestThread(\n", 1)[0]
        self.assertIn("guest_invalidation_completion_cv.notify_all();", terminal)
        self.assertNotIn("guest_invalidation_completion_cv.notify_one();", terminal)


if __name__ == "__main__":
    unittest.main()
