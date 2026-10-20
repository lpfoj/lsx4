import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
HOST = (ROOT / "src/executor/dynamic_translation/host_isa_profile.cpp").read_text()
RUNTIME = (ROOT / "src/executor/native_runtime_api.cpp").read_text()
ABI = (ROOT / "src/executor/fex_backend_abi_v3.h").read_text()
ADAPTER_ROOT = ROOT.parent / "refs/PS4Run/fex-2608-adapter-v3"
ADAPTER = (ADAPTER_ROOT / "src/adapter.cpp").read_text()
BRIDGE = (ADAPTER_ROOT / "src/fex_context_bridge.cpp").read_text()


class FexRpresCodegenContract(unittest.TestCase):
    def test_host_feature_requires_kernel_hwcap(self):
        self.assertIn("HWCAP2_RPRES", HOST)
        self.assertIn("HostIsaFeature::Rpres", HOST)

    def test_candidate_is_default_off_and_capability_gated(self):
        self.assertIn("run-enable-fex-rpres-codegen-ab", RUNTIME)
        self.assertIn("fex_rpres_requested && fex_rpres_supported", RUNTIME)
        self.assertIn("preflight.host_isa_feature_mask &= ~LSX4_FEX_HOST_ISA_RPRES", RUNTIME)

    def test_abi_and_adapter_preserve_double_detection(self):
        self.assertIn("LSX4_FEX_HOST_ISA_RPRES", ABI)
        self.assertIn("allowed_hwcap2 & HWCAP2_RPRES", ADAPTER)
        self.assertIn("detected & g_state.host_isa_features", ADAPTER)
        self.assertIn("features.SupportsRPRES", BRIDGE)

    def test_no_title_or_gpu_routing(self):
        runtime_candidate = RUNTIME.split(
            "const bool fex_rpres_requested", 1
        )[1].split("const bool proof_metrics_static", 1)[0]
        adapter_candidate = "\n".join(
            line for line in ADAPTER.splitlines() if "RPRES" in line
        )
        bridge_candidate = "\n".join(
            line for line in BRIDGE.splitlines() if "RPRES" in line
        )
        joined = runtime_candidate + adapter_candidate + bridge_candidate
        for token in ("CUSA", "Bloodborne", "Adreno", "Mali", "Qualcomm"):
            self.assertNotIn(token, joined)


if __name__ == "__main__":
    unittest.main()
