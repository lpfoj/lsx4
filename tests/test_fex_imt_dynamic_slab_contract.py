import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
NATIVE = (ROOT / "src" / "executor" / "native_runtime_api.cpp").read_text(
    encoding="utf-8"
)


def section(source: str, start: str, end: str) -> str:
    return source.split(start, 1)[1].split(end, 1)[0]


class FexImtDynamicSlabContractTests(unittest.TestCase):
    def test_slab_policy_is_truthful_rwx_self_modifying(self):
        policy = section(
            NATIVE,
            "static constexpr std::size_t kImtDynSlabSize",
            "static constexpr std::uint32_t kImtPoolN",
        )
        self.assertIn("4 * 1024 * 1024", policy)
        self.assertIn("kImtDynSlabCookieDomain", policy)
        self.assertIn("PROT_READ | PROT_WRITE | PROT_EXEC", policy)
        self.assertIn("LSX4_FEX_RANGE_EXECUTABLE", policy)
        self.assertIn("LSX4_FEX_RANGE_WRITABLE", policy)
        self.assertIn("LSX4_FEX_RANGE_SELF_MODIFYING", policy)

        setup = section(
            NATIVE,
            "static bool ExecutorMonoSetupImtPool()",
            "struct ManagedSpawnProbeSpec",
        )
        self.assertIn(
            "mmap(nullptr, kImtDynSlabSize, kImtDynSlabProtection", setup
        )

    def test_initial_mapped_entry_uses_rwx_and_stable_imt_cookie(self):
        source_segments = section(
            NATIVE,
            "std::uint32_t extra_index = mapped_segment_count;",
            "bool EnsureBox64GuestThreadStack",
        )
        imt_segment = section(
            source_segments,
            "if (imt_dyn_slab_base && imt_dyn_slab_size)",
            "return true;",
        )
        self.assertIn("segment.prot = PF_READ_WRITE_EXEC", imt_segment)

        request = section(
            NATIVE,
            "bool BuildExecutorFexMappedEntryRequest(",
            "extern \"C\" int executor_lsx4_runtime_run_mapped_entry",
        )
        self.assertIn("const bool is_imt_slab", request)
        self.assertIn("segment.size == kImtDynSlabSize", request)
        self.assertIn(
            "kImtDynSlabCookieDomain, segment.address, segment.size", request
        )
        self.assertIn("LSX4_FEX_RANGE_SELF_MODIFYING", request)

    def test_late_creation_publishes_whole_slab_before_ready(self):
        publish = section(
            NATIVE,
            "static bool ExecutorPublishImtDynamicSlabForActiveFex()",
            "static bool ExecutorCommitImtDynamicCode",
        )
        self.assertIn("Executor::Fex::ActiveApi() == nullptr", publish)
        self.assertIn("deferred_initial_snapshot", publish)
        self.assertIn(
            "executor_lsx4_android_fex_publish_dynamic_executable_range", publish
        )
        self.assertIn("kImtDynSlabProtection, kImtDynSlabRangeFlags", publish)

        setup = section(
            NATIVE,
            "static bool ExecutorMonoSetupImtPool()",
            "struct ManagedSpawnProbeSpec",
        )
        clear = setup.rindex("__builtin___clear_cache")
        late_publish = setup.index("ExecutorPublishImtDynamicSlabForActiveFex", clear)
        ready = setup.index(
            "g_imt_pool_ready.store(true, std::memory_order_release)", late_publish
        )
        self.assertLess(clear, late_publish)
        self.assertLess(late_publish, ready)
        self.assertIn("munmap(g_imt_dyn_slab, kImtDynSlabSize)", setup)

    def test_post_activation_commit_clears_then_invalidates_without_remove(self):
        commit = section(
            NATIVE,
            "static bool ExecutorCommitImtDynamicCode",
            "static bool ExecutorImtDynamicSlabContainsExecutableAddress",
        )
        clear = commit.index("__builtin___clear_cache")
        invalidate = commit.index(
            "executor_lsx4_android_fex_invalidate_dynamic_executable_range"
        )
        self.assertLess(clear, invalidate)
        self.assertIn("LSX4_FEX_RANGE_INVALIDATE_TRANSLATIONS", commit)
        self.assertNotIn("LSX4_FEX_RANGE_REMOVE_REGISTRATIONS", commit)
        self.assertIn("code_begin - slab_begin", commit)

    def test_every_append_only_code_emitter_uses_commit_barrier(self):
        required_sites = (
            "environment_newline_gate",
            "managed_spawn_probe_wrapper",
            "managed_spawn_probe_body",
            "managed_spawn_probe_trampoline",
            "managed_aot_probe_body",
            "managed_aot_probe_trampoline",
            "mono_class_image_repair",
            "mono_class_image_late_repair",
            "mono_current_thread_sidecar",
            "mono_reflection_gc_repair",
            "mono_assert_probe",
            "mono_generic_method_probe",
            "mono_method_full_name_guard",
            "mono_icall_noop_stub",
            "mono_icall_bind_detour",
            "mono_icall_slot_noop_stub",
            "mono_icall_slot_fill",
            "imt_limp_stub",
            "mono_vcall_entry_probe",
            "mono_vtnull_probe",
            "mono_value_copy_probe",
        )
        for site in required_sites:
            with self.subTest(site=site):
                self.assertIn(f'"{site}"', NATIVE)

    def test_unresolved_unity_icall_is_not_silently_stubbed_by_default(self):
        slot_fill = section(
            NATIVE,
            "constexpr std::size_t kLicOff = 0x37350;",
            "(void)str_off;",
        )
        self.assertIn("bool isf_enabled = false", slot_fill)
        self.assertIn("run-enable-icall-slot-fill", slot_fill)
        self.assertIn("if (isf_enabled && s_lic_noop", slot_fill)
        self.assertNotIn("run-no-icall-slot-fill", slot_fill)

        marker = slot_fill.index("run-enable-icall-slot-fill")
        install = slot_fill.index("if (isf_enabled && s_lic_noop")
        self.assertLess(marker, install)

    def test_reverse_callback_classifier_is_backend_scoped_and_ready_gated(self):
        slab_classifier = section(
            NATIVE,
            "static bool ExecutorImtDynamicSlabContainsExecutableAddress",
            "static std::uint64_t ExecutorBuildEnvironmentNewLineGate",
        )
        self.assertIn(
            "g_imt_pool_ready.load(std::memory_order_acquire)", slab_classifier
        )
        self.assertIn("address - begin < kImtDynSlabSize", slab_classifier)

        classifier = section(
            NATIVE,
            "bool IsGuestExecutableAddress",
            "struct X86InstructionDecode",
        )
        backend_guard = "RuntimeGuestJitActive() || RuntimeGuestFexActive()"
        self.assertIn(backend_guard, classifier)
        slab = classifier.index("ExecutorImtDynamicSlabContainsExecutableAddress")
        funnel = classifier.index(
            "executor_lsx4_android_is_live_dynamic_guest_executable_address"
        )
        self.assertLess(slab, funnel)


if __name__ == "__main__":
    unittest.main()
