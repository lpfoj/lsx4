import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
EXCEPTION = (
    ROOT
    / "funnel-arm"
    / "src"
    / "core"
    / "libraries"
    / "kernel"
    / "threads"
    / "exception.cpp"
).read_text(encoding="utf-8")


def function_body(start: str, end: str) -> str:
    return EXCEPTION.split(start, 1)[1].split(end, 1)[0]


class FexSignalActionContractTests(unittest.TestCase):
    def test_query_returns_complete_guest_state_before_any_backend_branch(self):
        action = function_body(
            "s32 PS4_SYSV_ABI posix_sigaction", "PosixSignalHandler PS4_SYSV_ABI posix_signal"
        )
        self.assertIn("const int previous_flags = HandlerFlags[sig]", action)
        self.assertIn("const std::uint8_t previous_kind = HandlerKinds[sig]", action)
        self.assertIn("const Sigset previous_mask = HandlerMasks[sig]", action)
        self.assertIn("oact->sa_flags = previous_flags", action)
        self.assertIn("oact->sa_mask = previous_mask", action)
        self.assertIn("previous_kind == 2 || previous_kind == 3", action)

        android_query = action.index("if (act == nullptr)")
        android_backend = action.index("if (ExecutorJitSignalParityActive())")
        first_commit = action.index("CommitGuestSignalAction")
        self.assertLess(android_query, android_backend)
        self.assertLess(android_query, first_commit)

    def test_guest_action_mirror_keeps_handler_kind_flags_and_mask_together(self):
        self.assertIn("std::array<Sigset, 130> HandlerMasks{}", EXCEPTION)
        commit = function_body(
            "static void CommitGuestSignalAction", "#ifdef __ANDROID__"
        )
        self.assertIn("Handlers[sig] = handler", commit)
        self.assertIn("HandlerFlags[sig] = action.sa_flags", commit)
        self.assertIn("HandlerKinds[sig] = GuestSignalHandlerKind", commit)
        self.assertIn("HandlerMasks[sig] = action.sa_mask", commit)

    def test_active_fex_registers_orbis_signum_before_committing_local_state(self):
        action = function_body(
            "s32 PS4_SYSV_ABI posix_sigaction", "PosixSignalHandler PS4_SYSV_ABI posix_signal"
        )
        fex_path = action.split('TraceLiveSignal("sigaction_register"', 1)[1]
        call_start = fex_path.index(
            "backend_rc = executor_lsx4_android_register_guest_signal_action_v3("
        )
        call_end = fex_path.index("TraceLiveSignal", call_start)
        v3_call = fex_path[call_start:call_end]
        self.assertRegex(v3_call, re.compile(r"action_v3\(\s*sig,"))
        self.assertNotIn("native_sig", v3_call)
        self.assertNotIn("legacy_backend_signal", v3_call)

        failure_gate = fex_path.index("if (backend_rc != 0)", call_start)
        commit = fex_path.index("CommitGuestSignalAction(sig, *act)", failure_gate)
        self.assertLess(call_start, failure_gate)
        self.assertLess(failure_gate, commit)
        self.assertIn(
            "executor_lsx4_android_register_guest_signal_handler(\n"
            "                legacy_backend_signal",
            fex_path,
        )

    def test_fex_registration_is_selected_by_active_owner_not_weak_symbol_presence(self):
        active = function_body(
            "static bool ExecutorFexSignalApiActive", "static bool ExecutorIsAsyncDeliveredSegv"
        )
        self.assertIn("executor_lsx4_android_fault_signal_owner() == 2", active)
        self.assertIn("executor_lsx4_android_fex_signal_api_active() != 0", active)

        action = function_body(
            "s32 PS4_SYSV_ABI posix_sigaction", "PosixSignalHandler PS4_SYSV_ABI posix_signal"
        )
        self.assertIn("if (fex_signal_owner_active)", action)
        self.assertIn("if (!fex_signal_api_active ||", action)
        self.assertNotIn(
            "if (executor_lsx4_android_register_guest_signal_action_v3 != nullptr)",
            action,
        )

    def test_kernel_remove_does_not_preclear_action_mirrors(self):
        remove = function_body(
            "int PS4_SYSV_ABI sceKernelRemoveExceptionHandler",
            "int PS4_SYSV_ABI sceKernelRaiseException",
        )
        self.assertNotIn("Handlers[signum] =", remove)
        self.assertNotIn("HandlerFlags[signum] =", remove)
        self.assertNotIn("HandlerKinds[signum] =", remove)
        self.assertNotIn("HandlerMasks[signum] =", remove)
        self.assertIn("posix_sigaction(signum, &act, nullptr)", remove)


if __name__ == "__main__":
    unittest.main()
