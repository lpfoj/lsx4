import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
NATIVE = (ROOT / "src" / "executor" / "native_runtime_api.cpp").read_text(
    encoding="utf-8"
)
LINKER = (ROOT / "funnel-arm" / "src" / "core" / "linker.cpp").read_text(
    encoding="utf-8"
)
PROCESS = (
    ROOT
    / "funnel-arm"
    / "src"
    / "core"
    / "libraries"
    / "kernel"
    / "process.cpp"
).read_text(encoding="utf-8")


class FexDynamicModulePublicationContractTests(unittest.TestCase):
    def test_initial_and_live_module_ranges_share_stable_cookie_identity(self):
        helper = NATIVE.split(
            "constexpr std::uint64_t StableModuleExecutableRangeCookie", 1
        )[1].split("bool ContainsRangeCookie", 1)[0]
        self.assertIn("StableGlobalExecutableRangeCookie", helper)
        self.assertIn("UINT64_C(0x4d4f44554c455258)", helper)
        self.assertNotIn("index", helper)

        register = NATIVE.split("int RegisterBox64ModuleSegments(void* module_ptr) {", 1)[
            1
        ].split("ExecutorBox64MappedEntryRequest request", 1)[0]
        self.assertIn(
            "StableModuleExecutableRangeCookie(\n"
            "                    segment.address, segment.size)",
            register,
        )
        self.assertNotIn("static_cast<std::uint64_t>(index + 1) << 56", register)

        activation = NATIVE.split("bool BuildExecutorFexMappedEntryRequest(", 1)[1].split(
            'extern "C" int executor_lsx4_runtime_run_jit_mapped_entry()', 1
        )[0]
        self.assertIn(
            ": StableModuleExecutableRangeCookie(segment.address, segment.size)",
            activation,
        )
        self.assertIn("UINT64_C(0x46554e4e454c5258)", activation)
        self.assertNotIn("static_cast<std::uint64_t>(index + 1) << 56", activation)

    def test_fex_module_only_ranges_bypass_legacy_global_request_limit(self):
        register = NATIVE.split("int RegisterBox64ModuleSegments", 1)[1].split(
            "bool ProcessPendingLoadExecLocked", 1
        )[0]
        self.assertIn("if (RuntimeGuestJitActive())", register)
        self.assertIn("[EXECUTOR_JIT_REGISTER_MODULE] result=not_required", register)
        fex = register.index("if (RuntimeGuestFexActive())")
        legacy_request = register.index("ExecutorBox64MappedEntryRequest request")
        self.assertLess(fex, legacy_request)
        fex_body = register[fex:legacy_request]
        self.assertIn("module->GetModuleInfo()", fex_body)
        self.assertIn("bulk_register_executable_ranges_v5", fex_body)

    def test_sysmodule_transaction_publishes_and_patches_before_start(self):
        lifecycle = LINKER.split("s32 Linker::LoadAndStartModule", 1)[1].split(
            "Module* Linker::FindByAddress", 1
        )[0]
        loaded = lifecycle.index("LoadModule(path, true)")
        publish = lifecycle.index(
            "executor_lsx4_android_register_box64_module_segments(module)"
        )
        relocate = lifecycle.index("RelocateAnyImports(module)")
        patch = lifecycle.index(
            "executor_lsx4_android_patch_box64_hle_imports(module)"
        )
        start = lifecycle.index("module->Start")
        self.assertLess(loaded, publish)
        self.assertLess(publish, relocate)
        self.assertLess(relocate, patch)
        self.assertLess(patch, start)
        self.assertIn("const bool newly_loaded", lifecycle)
        self.assertIn("Module::LifecycleState::Started", lifecycle)
        self.assertIn("result=transport_failed", lifecycle)
        self.assertIn("if (register_status != 0)", lifecycle)
        self.assertIn("if (patch_status != 0)", lifecycle)

    def test_kernel_load_start_does_not_ignore_publication_failures(self):
        load = PROCESS.split(
            "executor_lsx4_android_register_box64_module_segments(module)", 1
        )[1].split("return rc >= 0 ? handle : -1", 1)[0]
        self.assertIn("if (register_status != 0)", load)
        self.assertIn("if (patch_status != 0)", load)
        self.assertIn("stage=publish_exec_ranges", load)
        self.assertIn("stage=patch_hle_imports", load)

    def test_dynamic_hle_patch_transport_fails_closed_on_unpatched_native_targets(self):
        hook = NATIVE.split(
            'extern "C" int executor_lsx4_android_patch_box64_hle_imports', 1
        )[1].split("std::string BuildHleTrampolineStatusJson", 1)[0]
        self.assertIn("g_box64_hle_trampoline_skipped == 0 ? 0 : -2", hook)
        self.assertIn("return status", hook)
        self.assertIn("result=%d", hook)

    def test_live_range_publication_serializes_and_retries_only_stale_generation(self):
        register = NATIVE.split("int RegisterBox64ModuleSegments", 1)[1].split(
            "bool ProcessPendingLoadExecLocked", 1
        )[0]
        fex = register.split("if (RuntimeGuestFexActive()) {", 1)[1].split(
            "ExecutorBox64MappedEntryRequest request", 1
        )[0]
        self.assertIn("g_fex_executable_range_mutation_mutex", fex)
        self.assertIn("MaxGenerationAttempts = 4", fex)
        self.assertIn("snapshot_executable_ranges_v5", fex)
        self.assertIn("bulk_register_executable_ranges_v5", fex)
        self.assertIn(
            "status != LSX4_FEX_STATUS_INVALID_STATE", fex
        )
        self.assertIn("result=stale_generation_retry", fex)
        self.assertLess(
            fex.index("g_fex_executable_range_mutation_mutex"),
            fex.index("snapshot_executable_ranges_v5", 200),
        )


if __name__ == "__main__":
    unittest.main()
