import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
EXECUTOR = (ROOT / "src/executor/native_runtime_api.cpp").read_text(encoding="utf-8")
STUBS_HEADER = (
    ROOT / "funnel-arm/src/core/aerolib/stubs.h"
).read_text(encoding="utf-8")
STUBS_SOURCE = (
    ROOT / "funnel-arm/src/core/aerolib/stubs.cpp"
).read_text(encoding="utf-8")
PROCESS = (
    ROOT / "funnel-arm/src/core/libraries/kernel/process.cpp"
).read_text(encoding="utf-8")
PIGLET = (
    ROOT / "funnel-arm/src/core/libraries/piglet/piglet_android.cpp"
).read_text(encoding="utf-8")


class FexHlePrewarmContractTests(unittest.TestCase):
    def test_typed_inventory_is_snapshotted_and_validated(self):
        self.assertIn("struct AndroidX64TypedHleTarget", STUBS_HEADER)
        self.assertIn("SnapshotAndroidX64TypedHleTargets", STUBS_HEADER)
        for token in (
            "g_android_x64_hle_abi_by_native",
            "record.conflicting",
            "record.descriptor.supported",
            "record.descriptor.invoke == nullptr",
            "record.descriptor.descriptor_id == 0",
        ):
            self.assertIn(token, STUBS_SOURCE)

    def test_box64_trampolines_are_reused_by_native_target(self):
        allocation = EXECUTOR.split(
            "std::uint64_t AllocateBox64HleTrampoline", 1
        )[1].split("bool PrewarmFexHleManifests", 1)[0]
        self.assertIn("g_box64_hle_trampoline_by_native.find", allocation)
        self.assertLess(
            allocation.index("g_box64_hle_trampoline_by_native.find"),
            allocation.index("g_box64_hle_manifest_sealed"),
        )
        self.assertIn("g_box64_hle_trampoline_by_native.emplace", allocation)

    def test_every_typed_target_is_reserved_before_both_manifests_seal(self):
        prewarm = EXECUTOR.split("bool PrewarmFexHleManifests", 1)[1].split(
            "std::uint64_t ResolveFexHleImportThunk", 1
        )[0]
        self.assertIn("SnapshotAndroidX64TypedHleTargets", prewarm)
        self.assertIn("GetAndroidX64HleStubForNative", prewarm)
        self.assertNotIn("AllocateBox64HleTrampoline", prewarm)

        prewarm_call = EXECUTOR.index(
            "if (!PrewarmFexHleManifests(hle_prewarm_error))"
        )
        refreshed_request = EXECUTOR.index(
            "if (!BuildMappedEntryRequestFromModule(*module, request, refreshed_error))",
            prewarm_call,
        )
        upstream_seal = EXECUTOR.index(
            "if (!Core::AeroLib::SealAndroidX64ResolvedHleGeneration",
            refreshed_request,
        )
        box64_seal = EXECUTOR.index(
            "SealBox64HleManifestGeneration();", upstream_seal
        )
        self.assertLess(
            prewarm_call,
            refreshed_request,
        )
        self.assertLess(refreshed_request, upstream_seal)
        self.assertLess(upstream_seal, box64_seal)
        self.assertIn('"state":"fex_hle_prewarm_failed"', EXECUTOR[prewarm_call:refreshed_request])

    def test_dynamic_only_dlsym_targets_are_materialized_before_inventory_snapshot(self):
        curl_catalog = PROCESS.split(
            "constexpr std::array<AndroidCurlHleTarget, 11> AndroidCurlHleTargets", 1
        )[1].split("}};", 1)[0]
        self.assertEqual(curl_catalog.count('{"curl_'), 11)

        curl_resolver = PROCESS.split("void* ResolveAndroidCurlSymbol", 1)[1].split(
            "s32 TryLoadAndroidSyntheticModule", 1
        )[0]
        self.assertIn("for (const auto& target : AndroidCurlHleTargets)", curl_resolver)
        self.assertIn("target.name, target.function", curl_resolver)

        curl_prewarm = PROCESS.split(
            "bool PrewarmAndroidCurlDlsymHleTargets", 1
        )[1].split("s32 PS4_SYSV_ABI sceKernelIsInSandbox", 1)[0]
        self.assertIn("for (const auto& target : AndroidCurlHleTargets)", curl_prewarm)
        self.assertIn("GetAndroidX64HleStubForNative", curl_prewarm)

        piglet_prewarm = PIGLET.split(
            "bool PrewarmAndroidPigletDlsymHleTargets", 1
        )[1].split("void RegisterLib", 1)[0]
        self.assertIn('"orbisGlSwapBuffers", &OrbisGlSwapBuffers', piglet_prewarm)
        self.assertIn("*materialized_count = 1", piglet_prewarm)

        piglet_dynamic = set(
            re.findall(
                r'PIGLET_NAME\("[^"]+",\s*([A-Za-z0-9_]+)\);',
                PIGLET.split("void* ResolveSymbolByName", 1)[1].split(
                    "#undef PIGLET_NAME", 1
                )[0],
            )
        )
        piglet_registered = set(
            re.findall(
                r'PIGLET\("[^"]+",\s*([A-Za-z0-9_]+)\);',
                PIGLET.split("void RegisterLib", 1)[1].split("#undef PIGLET", 1)[0],
            )
        )
        self.assertEqual(len(piglet_dynamic), 88)
        self.assertEqual(piglet_dynamic - piglet_registered, {"OrbisGlSwapBuffers"})

        prewarm = EXECUTOR.split("bool PrewarmFexHleManifests", 1)[1].split(
            "std::uint64_t ResolveFexHleImportThunk", 1
        )[0]
        curl = prewarm.index("PrewarmAndroidCurlDlsymHleTargets")
        piglet = prewarm.index("PrewarmAndroidPigletDlsymHleTargets")
        snapshot = prewarm.index("SnapshotAndroidX64TypedHleTargets")
        self.assertLess(curl, snapshot)
        self.assertLess(piglet, snapshot)
        self.assertIn("ExpectedCurlDlsymTargets = 11", prewarm)
        self.assertIn("ExpectedPigletDlsymTargets = 1", prewarm)

    def test_executor_override_catalog_is_materialized_before_inventory_snapshot(self):
        self.assertIn("PrewarmAndroidX64ExecutorOverrides", STUBS_HEADER)
        catalog = STUBS_SOURCE.split(
            "const auto& GetExecutorStubOverrides", 1
        )[1].split("const ExecutorStubOverride* FindExecutorStubOverride", 1)[0]
        self.assertIn('{"tsvEmnenz48", "__cxa_atexit", &ExecutorCxaAtexit}', catalog)

        override_prewarm = STUBS_SOURCE.split(
            "bool PrewarmAndroidX64ExecutorOverrides", 1
        )[1].split("u64 GetAndroidX64HleStubForNative", 1)[0]
        self.assertIn("for (const auto& catalog_entry : GetExecutorStubOverrides())", override_prewarm)
        self.assertIn("TryGetAndroidX64ExecutorOverride", override_prewarm)
        self.assertIn("address == 0", override_prewarm)

        prewarm = EXECUTOR.split("bool PrewarmFexHleManifests", 1)[1].split(
            "std::uint64_t ResolveFexHleImportThunk", 1
        )[0]
        overrides = prewarm.index("PrewarmAndroidX64ExecutorOverrides")
        snapshot = prewarm.index("SnapshotAndroidX64TypedHleTargets")
        self.assertLess(overrides, snapshot)
        self.assertIn("executorOverrides=%zu", prewarm)

    def test_executor_override_never_reports_success_with_a_zero_stub(self):
        resolver = STUBS_SOURCE.split(
            "bool TryGetAndroidX64ExecutorOverride", 1
        )[1].split("bool PrewarmAndroidX64ExecutorOverrides", 1)[0]
        get_stub = resolver.index("const u64 resolved = GetStub(nid)")
        zero_guard = resolver.index("if (resolved == 0)", get_stub)
        failure = resolver.index("return false", zero_guard)
        assignment = resolver.index("*virtual_address = resolved", failure)
        self.assertLess(get_stub, zero_guard)
        self.assertLess(zero_guard, failure)
        self.assertLess(failure, assignment)

    def test_box64_capacity_matches_funnel_manifest_abi(self):
        self.assertIn("ExecutorBox64HleTrampolineSlots = 8192", EXECUTOR)

    def test_late_raw_native_import_reuses_canonical_funnel_stub(self):
        resolver = EXECUTOR.split("std::uint64_t ResolveFexHleImportThunk", 1)[1].split(
            "static const std::uint8_t kImtThunkTemplate38", 1
        )[0]
        self.assertIn("g_box64_hle_trampoline_by_native.find", resolver)
        self.assertIn("g_box64_hle_manifest_sealed", resolver)
        self.assertIn("GetAndroidX64HleStubForNative", resolver)
        self.assertIn("EXECUTOR_FEX_HLE_CANONICAL_MISS", resolver)
        install = EXECUTOR.split("void InstallBox64HleTrampolines", 1)[1].split(
            'extern "C" int executor_lsx4_android_patch_box64_hle_imports', 1
        )[0]
        self.assertEqual(install.count("ResolveFexHleImportThunk"), 2)


if __name__ == "__main__":
    unittest.main()
