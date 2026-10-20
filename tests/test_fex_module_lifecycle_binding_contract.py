import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
NATIVE_RUNTIME = (
    ROOT / "src" / "executor" / "native_runtime_api.cpp"
).read_text(encoding="utf-8")
MODULE_HEADER = (
    ROOT / "funnel-arm" / "src" / "core" / "module.h"
).read_text(encoding="utf-8")
MODULE_SOURCE = (
    ROOT / "funnel-arm" / "src" / "core" / "module.cpp"
).read_text(encoding="utf-8")
FEX_ADAPTER = (
    ROOT.parent
    / "refs"
    / "PS4Run"
    / "fex-2608-adapter-v3"
    / "src"
    / "fex_context_bridge.cpp"
).read_text(encoding="utf-8")


class FexModuleLifecycleBindingContractTests(unittest.TestCase):
    def test_live_module_lifecycle_reuses_nested_fex_binding(self):
        lifecycle = NATIVE_RUNTIME.split(
            "int RunFexGuestModuleLifecycle", 1
        )[1].split("int DeferFexBootstrapModuleLifecycle", 1)[0]
        self.assertIn("ScopedFexCallbackBinding binding{api}", lifecycle)
        self.assertIn("const int bind_status = binding.status()", lifecycle)
        self.assertNotIn("api->bind_host_thread(", lifecycle)
        self.assertNotIn("api->unbind_host_thread(", lifecycle)
        self.assertIn("api->set_guest_tls", lifecycle)
        snapshot = lifecycle.index("api->get_guest_tls(")
        restore = lifecycle.index("ScopedFexTlsRestore")
        module_tls = lifecycle.index("tls.tls_image_address")
        callback = lifecycle.index("RunFexGuestCallback3")
        self.assertLess(snapshot, restore)
        self.assertLess(restore, module_tls)
        self.assertLess(module_tls, callback)
        self.assertIn("result=tls_restore_failed", lifecycle)

    def test_scoped_binding_probes_before_acquiring_and_only_releases_owner(self):
        scoped = NATIVE_RUNTIME.split(
            "class ScopedFexCallbackBinding", 1
        )[1].split("int RunFexGuestCallbackN", 1)[0]
        probe = scoped.index("api_->get_guest_tls")
        acquire = scoped.index("api_->bind_host_thread")
        own = scoped.index("owns_binding_ = true", acquire)
        self.assertLess(probe, acquire)
        self.assertLess(acquire, own)
        self.assertIn(
            "probe_status != LSX4_FEX_STATUS_INVALID_ARGUMENT", scoped
        )
        self.assertIn(
            "probe_status != LSX4_FEX_STATUS_INVALID_STATE", scoped
        )
        self.assertIn("probe_status != LSX4_FEX_STATUS_NOT_FOUND", scoped)
        self.assertIn("if (owns_binding_)", scoped)

    def test_nested_callback_stacks_below_and_restores_hle_state(self):
        callback = NATIVE_RUNTIME.split(
            "int RunFexGuestCallbackN", 1
        )[1].split("int RunFexGuestCallback3", 1)[0]
        self.assertIn("request.guest_rsp = 0", callback)
        self.assertIn("start below the current", callback)
        self.assertIn("callback scratch stack", callback)
        self.assertNotIn("CurrentExecutorHleGuestRsp", NATIVE_RUNTIME)

        adapter_callback = FEX_ADAPTER.split(
            "int32_t RunGuestCallback", 1
        )[1].split("int32_t RunMappedEntry", 1)[0]
        snapshot = adapter_callback.index("CPUState nested_prior_state")
        overwrite = adapter_callback.index("if (request->guest_rsp != 0)")
        result_extract = adapter_callback.index(
            "result->rax = state.gregs[FEXCore::X86State::REG_RAX]"
        )
        self.assertLess(snapshot, overwrite)
        self.assertLess(overwrite, result_extract)
        self.assertIn("struct NestedGuestStateRestore", adapter_callback)
        self.assertRegex(
            adapter_callback,
            r"&nested_prior_state,\s*!owns_activity",
        )
        self.assertIn("record->nonlocal_pending", adapter_callback)
        self.assertIn(
            "record->context_epoch.load(std::memory_order_acquire)",
            adapter_callback,
        )
        self.assertIn(
            "std::memcpy(&record->thread->CurrentFrame->State, prior_state",
            adapter_callback,
        )
        self.assertNotIn("nested_prior_stack", adapter_callback)
        self.assertNotIn("callback-stack", adapter_callback)
        for preserved in (
            "RIP, register file, flags, vector state, TLS bases",
            "deferred-signal",
            "call/return shadow-stack pointer",
        ):
            self.assertIn(preserved, adapter_callback)

    def test_zero_offset_dt_init_is_the_nonduplicating_canonical_owner(self):
        self.assertIn("bool has_init = false", MODULE_HEADER)
        parser = MODULE_SOURCE.split("void Module::LoadDynamicInfo", 1)[1]
        dt_init = parser.split("case DT_INIT:", 1)[1].split("break;", 1)[0]
        self.assertIn("dynamic_info.has_init = true", dt_init)
        self.assertIn("dynamic_info.init_virtual_addr = dyn->d_un.d_ptr", dt_init)

        lifecycle = NATIVE_RUNTIME.split(
            "int RunFexGuestModuleLifecycle", 1
        )[1].split("int DeferFexBootstrapModuleLifecycle", 1)[0]
        run_function = lifecycle.split("auto run_function", 1)[1].split(
            "auto run_array", 1
        )[0]
        self.assertNotIn("function == module.GetBaseAddress()", run_function)
        self.assertIn("if (module.dynamic_info.has_init)", lifecycle)
        self.assertIn(
            "module.GetBaseAddress() + module.dynamic_info.init_virtual_addr",
            lifecycle,
        )
        self.assertNotIn(
            "ran_any = module.dynamic_info.preinit_array_size != 0", lifecycle
        )
        self.assertNotIn(
            "ran_any = ran_any || module.dynamic_info.init_array_size != 0",
            lifecycle,
        )
        self.assertIn("bool* const executed_any", lifecycle)
        self.assertIn("*executed_any = true", lifecycle)
        self.assertNotIn("reason=array_lifecycle_executed", lifecycle)
        self.assertIn("result=non_executable_dt_init", lifecycle)
        canonical = lifecycle.index("if (module.dynamic_info.has_init)")
        preinit_array = lifecycle.index(
            "module.dynamic_info.preinit_array_virtual_addr", canonical
        )
        init_array = lifecycle.index(
            "module.dynamic_info.init_array_virtual_addr", preinit_array
        )
        symbol_fallback = lifecycle.index("if (!ran_any)", init_array)
        self.assertLess(canonical, preinit_array)
        self.assertLess(preinit_array, init_array)
        self.assertLess(init_array, symbol_fallback)

        canonical_end = lifecycle.index(
            "        ran_any = true;\n    } else {", canonical
        )
        canonical_branch = lifecycle[canonical:canonical_end]
        self.assertIn('"dt_init", 0', canonical_branch)
        self.assertNotIn("run_array(", canonical_branch)



if __name__ == "__main__":
    unittest.main()
