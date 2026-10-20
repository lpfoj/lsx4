import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
ABI = (ROOT / "src/executor/fex_backend_abi_v3.h").read_text(encoding="utf-8")
LOADER_H = (ROOT / "src/executor/fex_backend_loader.h").read_text(
    encoding="utf-8"
)
LOADER = (ROOT / "src/executor/fex_backend_loader.cpp").read_text(
    encoding="utf-8"
)
NATIVE = (ROOT / "src/executor/native_runtime_api.cpp").read_text(
    encoding="utf-8"
)


class FexRelaxedMemoryAbContractTests(unittest.TestCase):
    def test_append_only_abi_uses_existing_configure_field(self):
        self.assertIn(
            "LSX4_FEX_CONFIG_RELAX_VECTOR_TSO_AB = UINT32_C(1) << 0", ABI
        )
        self.assertIn(
            "LSX4_FEX_CONFIG_RELAX_MEMCPY_SET_TSO_AB = UINT32_C(1) << 1",
            ABI,
        )
        self.assertIn(
            "LSX4_FEX_CAP_MEMORY_MODEL_AB_OVERRIDES = UINT64_C(1) << 12", ABI
        )
        self.assertIn("LSX4_FEX_ABI_ASSERT_POD(Lsx4FexConfigureRequestV3, 128)", ABI)
        self.assertIn("std::uint32_t configure_flags{}", LOADER_H)

    def test_cpu_profile_is_authoritative_over_legacy_memory_markers(self):
        vector = NATIVE.index('root / "run-enable-fex-relaxed-vector-tso-ab"')
        memcpy = NATIVE.index('root / "run-enable-fex-relaxed-memcpyset-tso-ab"')
        stable = NATIVE.index('root / "run-sm8650-fex-gamehub-stable-memory-ab"')
        profile = NATIVE.index("const bool cpu_fast_atomics", stable)
        preflight = NATIVE.index("Executor::Fex::Preflight(preflight)", profile)
        self.assertLess(vector, memcpy)
        self.assertLess(memcpy, stable)
        self.assertLess(stable, profile)
        self.assertLess(profile, preflight)
        self.assertIn("std::filesystem::is_regular_file", NATIVE[vector - 100 : memcpy])
        self.assertIn("std::filesystem::is_regular_file", NATIVE[memcpy - 100 : stable])
        self.assertIn("legacyStableMarkerIgnored=1", NATIVE[profile:preflight])
        self.assertIn(
            "LSX4_FEX_CONFIG_RELAX_VECTOR_TSO_AB |",
            NATIVE[profile:preflight],
        )
        self.assertIn(
            "LSX4_FEX_CONFIG_RELAX_MEMCPY_SET_TSO_AB",
            NATIVE[profile:preflight],
        )
        self.assertIn("preflight.configure_flags", NATIVE[profile:preflight])
        self.assertIn("[EXECUTOR_FEX_MEMORY_MODEL_AB]", NATIVE[vector:preflight])

    def test_loader_uses_dynamic_capability_contract(self):
        self.assertIn("kKnownMemoryModelAbFlags", LOADER)
        self.assertIn("invalid-configure-flags", LOADER)
        self.assertIn('"-ab-vm-tso0"', LOADER)
        self.assertIn(
            "relaxed-memory-ab-conflicts-with-explicit-strong-memory", LOADER
        )
        self.assertIn("relaxed-memory-ab-requires-software-tso", LOADER)
        self.assertIn("required &= ~LSX4_FEX_CAP_STRONG_MEMORY", LOADER)
        self.assertIn("required |= LSX4_FEX_CAP_MEMORY_MODEL_AB_OVERRIDES", LOADER)
        self.assertIn("configure.configure_flags = request.configure_flags", LOADER)
        self.assertIn("state.activation_required_capabilities = required", LOADER)
        self.assertIn(
            "state.activation_forbidden_capabilities", LOADER
        )
        stage = LOADER.split("bool StageActivation(", 1)[1].split(
            "bool CommitActivation()", 1
        )[0]
        self.assertIn("state.activation_required_capabilities", stage)
        self.assertIn("state.activation_forbidden_capabilities", stage)
        self.assertNotIn(
            "(result.effective_capabilities & kRequiredPs4Capabilities)", stage
        )

    def test_platform_contract_is_additive_and_preflight_is_immutable(self):
        self.assertRegex(
            LOADER,
            r"kRequiredPlatformCapabilities\s*\|\s*request\.required_capabilities",
        )
        self.assertRegex(
            LOADER,
            r"request\.platform_mask\s*&\s*\(LSX4_FEX_PLATFORM_PS4\s*\|\s*"
            r"LSX4_FEX_PLATFORM_PS5\)",
        )
        self.assertIn("SamePreflightRequest", LOADER)
        self.assertIn('"preflight-request-mismatch"', LOADER)
        self.assertIn("StrandActivation(state, failure_code", LOADER)

    def test_status_publishes_effective_memory_contract(self):
        for field in (
            '"configureFlags"',
            '"providedCapabilities"',
            '"requiredCapabilities"',
            '"forbiddenCapabilities"',
            '"strongMemoryModel"',
        ):
            self.assertIn(field, NATIVE)
        self.assertIn("fex-preflight-stranded-restart-required", NATIVE)

    def test_activation_stranded_is_process_lifetime_poison(self):
        fail = LOADER.split("void Fail(", 1)[1].split("void StrandActivation(", 1)[0]
        stage = LOADER.split("bool StageActivation(", 1)[1].split(
            "bool CommitActivation()", 1
        )[0]
        commit = LOADER.split("bool CommitActivation()", 1)[1].split(
            "LoaderStatus Status()", 1
        )[0]
        self.assertIn("LoaderState::ActivationStranded", fail)
        self.assertIn("LoaderState::ActivationStranded", stage)
        self.assertIn("LoaderState::ActivationStranded", commit)

        setter = NATIVE.split("int SetGuestCpuBackend(", 1)[1].split(
            'extern "C" int executor_lsx4_runtime_set_guest_cpu_backend', 1
        )[0]
        initialize = NATIVE.split(
            'extern "C" int executor_lsx4_runtime_initialize', 1
        )[1]
        self.assertLess(
            setter.index("LoaderState::ActivationStranded"),
            setter.index("requested == Executor::GuestCpuBackend::Current"),
        )
        self.assertIn("SelectStrandedFexGuestCpuBackendLocked", setter)
        self.assertIn("LoaderState::ActivationStranded", initialize[:5000])
        self.assertIn("SelectStrandedFexGuestCpuBackendLocked", initialize[:5000])

    def test_baseline_namespace_stays_compatible(self):
        namespace = LOADER.split("const char* const memory_model_suffix", 1)[1].split(
            "Lsx4FexConfigureRequestV3 configure", 1
        )[0]
        self.assertIn('"-ab-vtso0"', namespace)
        self.assertIn('"-ab-mtso0"', namespace)
        self.assertIn(': ""', namespace)
        self.assertIn('hardware_tso ? "-sm-hwtso" : "-sm-swtso"', namespace)


if __name__ == "__main__":
    unittest.main()
