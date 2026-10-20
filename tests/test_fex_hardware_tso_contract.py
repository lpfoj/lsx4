from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
HOST_PROFILE = (
    ROOT / "src/executor/dynamic_translation/host_isa_profile.cpp"
).read_text(encoding="utf-8")
LOADER = (ROOT / "src/executor/fex_backend_loader.cpp").read_text(
    encoding="utf-8"
)
NATIVE = (ROOT / "src/executor/native_runtime_api.cpp").read_text(
    encoding="utf-8"
)
BRIDGE = (
    ROOT.parent / "refs/PS4Run/fex-2608-adapter-v3/src/fex_context_bridge.cpp"
).read_text(encoding="utf-8")


class FexHardwareTsoContract(unittest.TestCase):
    def test_executor_uses_a_noncached_per_thread_set(self):
        body = HOST_PROFILE.split(
            "HardwareTsoAttempt TryEnableHardwareTsoForCurrentThread() noexcept", 1
        )[1].split("const HostIsaProfile& GetHostIsaProfile", 1)[0]
        self.assertIn("prctl(PR_GET_MEM_MODEL", body)
        self.assertIn("prctl(PR_SET_MEM_MODEL, PR_SET_MEM_MODEL_TSO", body)
        self.assertIn("attempt.enabled = attempt.set_result == 0", body)
        self.assertIn("const int saved_errno = errno", body)
        self.assertIn("errno = saved_errno", body)
        self.assertNotIn("static const HardwareTsoAttempt", body)

    def test_loader_never_advertises_cached_hardware_tso(self):
        clear = LOADER.index("effective_host_os_features &= ~kHardwareTso")
        enable = LOADER.index("Jit::TryEnableHardwareTsoForCurrentThread()")
        restore = LOADER.index("Jit::RestoreHardwareTsoAfterProbe(tso_attempt)")
        configure = LOADER.index("configure.host_os_features = effective_host_os_features")
        self.assertLess(clear, enable)
        self.assertLess(enable, restore)
        self.assertLess(restore, configure)
        self.assertLess(enable, configure)
        self.assertIn('hardware_tso ? "-sm-hwtso" : "-sm-swtso"', LOADER)

    def test_explicit_software_ab_marker_clears_the_request_bit(self):
        marker = NATIVE.index('root / "run-force-fex-software-tso"')
        clear = NATIVE.index(
            "~LSX4_FEX_HOST_OS_HARDWARE_TSO_ENABLED", marker
        )
        preflight = NATIVE.index("Executor::Fex::Preflight(preflight)", clear)
        self.assertLess(marker, clear)
        self.assertLess(clear, preflight)

    def test_bridge_rechecks_before_context_initialization(self):
        create = BRIDGE.split("int32_t CreateRuntime(", 1)[1]
        enable = create.index("TryEnableHardwareTsoForCurrentThread()")
        restore = create.index("RestoreHardwareTsoAfterProbe(", enable)
        advertise = create.index("SetHardwareTSOSupport(")
        init = create.index("InitCore()")
        self.assertLess(enable, restore)
        self.assertLess(enable, advertise)
        self.assertLess(advertise, init)

    def test_every_hardware_bind_is_fail_closed_before_record_publication(self):
        bind = BRIDGE.split("int32_t BindHostThread(", 1)[1].split(
            "int32_t UnbindHostThread(", 1
        )[0]
        enable = bind.index("TryEnableHardwareTsoForCurrentThread()")
        failure = bind.index("return LSX4_FEX_STATUS_BACKEND_FAILURE", enable)
        records_lock = bind.index("runtime->records_mutex")
        persistent = bind.index("CreatePersistentThread")
        poison = bind.index("hardware_tso_bind_failed.store", enable)
        fatal = bind.index("runtime->callbacks.fatal_error(", poison)
        self.assertLess(enable, failure)
        self.assertLess(poison, fatal)
        self.assertLess(fatal, failure)
        self.assertLess(failure, records_lock)
        self.assertLess(records_lock, persistent)
        helper = BRIDGE.split(
            "HardwareTsoAttempt TryEnableHardwareTsoForCurrentThread() noexcept", 1
        )[1].split("void LogHardwareTsoAttempt", 1)[0]
        self.assertIn("const int saved_errno = errno", helper)
        self.assertGreaterEqual(helper.count("errno = saved_errno"), 3)

    def test_unbind_does_not_reset_the_memory_model(self):
        unbind = BRIDGE.split("int32_t UnbindHostThread(", 1)[1].split(
            "bool ReleaseIdentityMatches", 1
        )[0]
        self.assertNotIn("PR_SET_MEM_MODEL", unbind)
        self.assertNotIn("PR_SET_MEM_MODEL_DEFAULT", unbind)


if __name__ == "__main__":
    unittest.main()
