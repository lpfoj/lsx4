import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
RUNTIME = (ROOT / "src/executor/native_runtime_api.cpp").read_text(
    encoding="utf-8"
)


class FexLseAtomicsAbContractTests(unittest.TestCase):
    def test_candidate_is_default_off_and_preserves_strict_memory(self):
        self.assertIn("run-disable-fex-lse-atomics-ab", RUNTIME)
        self.assertIn(
            "preflight.host_isa_feature_mask &= ~LSX4_FEX_HOST_ISA_LSE",
            RUNTIME,
        )
        candidate = RUNTIME.split(
            "const auto disable_fex_lse_atomics_marker", 1
        )[1].split("if ((cpu_disabled_isa", 1)[0]
        self.assertIn("memoryModel=strict", candidate)
        self.assertNotIn("RELAX_VECTOR_TSO", candidate)
        self.assertNotIn("RELAX_MEMCPY_SET_TSO", candidate)

    def test_candidate_is_not_title_or_gpu_specific(self):
        candidate = RUNTIME.split(
            "const auto disable_fex_lse_atomics_marker", 1
        )[1].split("if ((cpu_disabled_isa", 1)[0]
        for token in ("CUSA", "Exit 8", "Adreno", "Mali", "Qualcomm"):
            self.assertNotIn(token, candidate)


if __name__ == "__main__":
    unittest.main()
