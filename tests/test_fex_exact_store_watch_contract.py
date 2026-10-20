import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
WORKSPACE = ROOT.parent
FEX_MEMORY = (
    WORKSPACE
    / "refs/PS4Run/repos/fex-emu-2608/FEXCore/Source/Interface/Core/JIT/MemoryOps.cpp"
).read_text(encoding="utf-8")
BRIDGE = (
    WORKSPACE / "refs/PS4Run/fex-2608-adapter-v3/src/fex_context_bridge.cpp"
).read_text(encoding="utf-8")
TRACE = (
    WORKSPACE / "refs/PS4Run/fex-2608-adapter-v3/src/guest_store_trace_ir.cpp"
).read_text(encoding="utf-8")
NATIVE = (ROOT / "src/executor/native_runtime_api.cpp").read_text(
    encoding="utf-8"
)


class FexExactStoreWatchContractTests(unittest.TestCase):
    def test_marker_instruments_every_scalar_store_lowering_without_rip_hardcode(self):
        gate = FEX_MEMORY.split(
            "bool ExactStoreWatchInstructionEnabled", 1
        )[1].split("void Arm64JITCore::EmitExactStoreWatch", 1)[0]
        self.assertIn("return ExactStoreWatchCompilationEnabled()", gate)
        self.assertNotIn("ProducerBegin", gate)
        self.assertNotIn("ProducerEnd", gate)
        self.assertNotRegex(gate, r"0x80[0-9a-fA-F]{7,}")
        self.assertNotIn("EmitExactStoreValueTrigger", FEX_MEMORY)
        self.assertNotIn("kExtent960x544", FEX_MEMORY)

        for lowering in ("DEF_OP(StoreMem)", "DEF_OP(StoreMemPair)", "DEF_OP(StoreMemTSO)"):
            body = FEX_MEMORY.split(lowering, 1)[1].split("\n}\n", 1)[0]
            self.assertIn("ExactStoreWatchInstructionEnabled(CurrentGuestRIP)", body)
            self.assertIn("EmitExactStoreWatch", body)

    def test_unarmed_generated_path_checks_gate_before_stack_or_flags(self):
        emit = FEX_MEMORY.split(
            "void Arm64JITCore::EmitExactStoreWatch", 1
        )[1].split("void Arm64JITCore::EmitExactStoreWatchHit", 1)[0]
        self.assertLess(emit.index("range_armed"), emit.index("Reg::rsp"))
        self.assertLess(emit.index("cbz"), emit.index("SystemRegister::NZCV"))

    def test_marker_disables_only_diagnostic_persistent_host_code_cache(self):
        self.assertIn("bool ExactStoreWatchRequested() noexcept", TRACE)
        self.assertIn("diag-fex-pm4-header-store-watch", TRACE)
        cache = BRIDGE.split("if (runtime->exact_store_watch", 1)[1].split(
            "context_impl->Config.NeedsPendingInterruptFaultCheck", 1
        )[0]
        self.assertIn("EnableCodeCaching", cache)
        self.assertIn("EnableLazyCodeCaching", cache)
        self.assertIn("FEXCore::Config::Value<bool>{false}", cache)

    def test_pm4_marker_arms_address_range_before_first_guest_block(self):
        enable = NATIVE.split("bool EnableFexExactStoreWatchFromMarker", 1)[1].split(
            'extern "C" bool executor_lsx4_android_fex_exact_store_watch_arm', 1
        )[0]
        self.assertIn("kPm4DiagnosticBegin = UINT64_C(0x300000000)", enable)
        self.assertIn("kPm4DiagnosticEnd = UINT64_C(0x310000000)", enable)
        self.assertIn("ExactStoreWatchArmRange", enable)
        activation = NATIVE.split("if (!Executor::Fex::CommitActivation()", 1)[1]
        self.assertLess(
            activation.index("EnableFexExactStoreWatchFromMarker()"),
            activation.index("run_mapped_entry_v5"),
        )


if __name__ == "__main__":
    unittest.main()
