import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
WORKSPACE = ROOT.parent
BRIDGE = (
    WORKSPACE / "refs/PS4Run/fex-2608-adapter-v3/src/fex_context_bridge.cpp"
).read_text(encoding="utf-8")
BRANCH_OPS = (
    WORKSPACE
    / "refs/PS4Run/repos/fex-emu-2608/FEXCore/Source/Interface/Core/JIT/BranchOps.cpp"
).read_text(encoding="utf-8")
LINUX_FRONTEND = (
    WORKSPACE
    / "refs/PS4Run/repos/fex-emu-2608/Source/Tools/LinuxEmulation/LinuxSyscalls/SyscallsSMCTracking.cpp"
).read_text(encoding="utf-8")


class FexCallRetGuardRecoveryContractTests(unittest.TestCase):
    def test_upstream_requires_frontend_overflow_and_underflow_recovery(self):
        self.assertIn("guest may not balance calls and returns exactly", BRANCH_OPS)
        self.assertIn("reset the shadow stack", BRANCH_OPS)
        self.assertIn("CallRetStackInfo.AllocationBase", LINUX_FRONTEND)
        self.assertIn("SetArmReg(ucontext, 25", LINUX_FRONTEND)

    def test_bridge_predicate_accepts_only_the_two_guard_pages(self):
        predicate = BRIDGE.split(
            "bool IsCallRetStackGuardAddress", 1
        )[1].split("bool IsDeferredInterruptProbeStore", 1)[0]
        self.assertIn("page_size > record->callret_allocation_size / 2", predicate)
        self.assertIn("offset < page_size", predicate)
        self.assertIn(
            "offset >= record->callret_allocation_size - page_size", predicate
        )
        self.assertIn("offset < record->callret_allocation_size", predicate)

    def test_recovery_preserves_fail_closed_signal_order(self):
        handler = BRIDGE.split("int32_t HandleHostSignal", 1)[1].split(
            "bool AllocateSentinels", 1
        )[0]
        active = handler.index("record->active.load")
        jit_guard = handler.index("IsAddressInArm64JitGuardPage")
        deferred = handler.index("if (deferred_helper_active)")
        callret = handler.index("IsCallRetStackGuardAddress")
        capture = handler.index("EnterSignalHandler")
        self.assertLess(active, jit_guard)
        self.assertLess(jit_guard, deferred)
        self.assertLess(deferred, callret)
        self.assertLess(callret, capture)

    def test_recovery_is_single_register_async_signal_safe_retry(self):
        recovery = BRIDGE.split(
            "// FEX's call/return predictor", 1
        )[1].split("#if defined(__aarch64__)", 1)[0]
        self.assertIn("si_code == SEGV_ACCERR", recovery)
        self.assertNotIn("SEGV_MAPERR", recovery)
        self.assertIn("uc_mcontext.regs[kCallRetStackPointerRegister]", recovery)
        self.assertIn("record->callret_reset_sp", recovery)
        self.assertIn("LSX4_FEX2608_HOST_SIGNAL_HANDLED", recovery)
        for forbidden in (
            "ResetCallRetShadowStack",
            "VirtualDontNeed",
            "madvise",
            "mprotect",
            "memcpy",
            "Log(",
            "mutex",
        ):
            self.assertNotIn(forbidden, recovery)


if __name__ == "__main__":
    unittest.main()
