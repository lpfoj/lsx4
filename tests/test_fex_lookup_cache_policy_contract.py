from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
NATIVE = (ROOT / "src/executor/native_runtime_api.cpp").read_text(
    encoding="utf-8"
)
BRIDGE = (
    ROOT.parent
    / "refs/PS4Run/fex-2608-adapter-v3/src/fex_context_bridge.cpp"
).read_text(encoding="utf-8")
MIDR_POLICY = (
    ROOT.parent
    / "refs/PS4Run/fex-2608-adapter-v3/src/host_midr_quirk.h"
).read_text(encoding="utf-8")


class FexLookupCachePolicyContract(unittest.TestCase):
    def test_marker_sets_the_bridge_environment_before_preflight(self):
        marker = NATIVE.index('root / "run-enable-fex-low-memory-cache"')
        set_environment = NATIVE.index(
            '::setenv("LSX4_FEX_LOW_MEMORY_CACHE", "1", 1)', marker
        )
        preflight = NATIVE.index("Executor::Fex::Preflight(preflight)", marker)
        self.assertLess(marker, set_environment)
        self.assertLess(set_environment, preflight)

    def test_absent_marker_clears_a_stale_process_environment(self):
        marker = NATIVE.index('root / "run-enable-fex-low-memory-cache"')
        clear_environment = NATIVE.index(
            '::unsetenv("LSX4_FEX_LOW_MEMORY_CACHE")', marker
        )
        preflight = NATIVE.index("Executor::Fex::Preflight(preflight)", marker)
        self.assertLess(marker, clear_environment)
        self.assertLess(clear_environment, preflight)

    def test_requested_and_effective_policies_have_unambiguous_logs(self):
        self.assertIn('"-lookup-lowmem" : "-lookup-throughput"', NATIVE)
        self.assertIn("[EXECUTOR_FEX_LOOKUP_CACHE]", NATIVE)
        self.assertIn(
            "effectiveLowMemory=%d requestedDynamicL1=%d ",
            NATIVE,
        )
        self.assertIn(
            "requestedDisableL2=%d effectivePolicy=bridge",
            NATIVE,
        )
        self.assertIn("[EXECUTOR_FEX_LOOKUP_CACHE_EFFECTIVE]", BRIDGE)
        self.assertIn("disableL2=%d dynamicL1=%d", BRIDGE)

    def test_bridge_selects_the_host_policy_before_setting_fex_config(self):
        policy = BRIDGE.split(
            'low_memory_cache = std::getenv("LSX4_FEX_LOW_MEMORY_CACHE")',
            1,
        )[1].split("g_process_page_size = page_size", 1)[0]
        select = policy.index("SelectLookupCachePolicy(prefer_low_memory")
        disable_l2 = policy.index("CONFIG_DISABLEL2CACHE")
        dynamic_l1 = policy.index("CONFIG_DYNAMICL1CACHE")
        self.assertLess(select, disable_l2)
        self.assertLess(select, dynamic_l1)
        self.assertIn("CONFIG_DISABLEL2CACHE", policy)
        self.assertIn("CONFIG_DYNAMICL1CACHE", policy)

    def test_low_memory_policy_remains_dynamic_l1_without_l2(self):
        self.assertIn("if (prefer_low_memory)", MIDR_POLICY)
        self.assertIn(
            "return {.disable_l2 = true, .dynamic_l1 = true};",
            MIDR_POLICY,
        )

    def test_cortex_x4_keeps_l2_and_uses_dynamic_l1(self):
        self.assertIn("if (has_cortex_x4)", MIDR_POLICY)
        self.assertIn(
            "return {.disable_l2 = false, .dynamic_l1 = true};",
            MIDR_POLICY,
        )

    def test_default_throughput_policy_is_unchanged(self):
        self.assertIn(
            "return {.disable_l2 = false, .dynamic_l1 = false};",
            MIDR_POLICY,
        )


if __name__ == "__main__":
    unittest.main()
