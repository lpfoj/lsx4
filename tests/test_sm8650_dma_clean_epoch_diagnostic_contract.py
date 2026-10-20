from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
VIDEO = ROOT / "funnel-arm" / "src" / "video_core"
RASTERIZER_H = (VIDEO / "renderer_vulkan" / "vk_rasterizer.h").read_text(
    encoding="utf-8"
)
RASTERIZER = (VIDEO / "renderer_vulkan" / "vk_rasterizer.cpp").read_text(
    encoding="utf-8"
)
BUFFER_H = (VIDEO / "buffer_cache" / "buffer_cache.h").read_text(
    encoding="utf-8"
)
BUFFER = (VIDEO / "buffer_cache" / "buffer_cache.cpp").read_text(
    encoding="utf-8"
)


class Sm8650DmaCleanEpochDiagnosticContract(unittest.TestCase):
    def test_gate_is_marker_title_and_two_exact_gpu_axes_only(self) -> None:
        gate = RASTERIZER.split(
            "ExecutorBloodborneDmaCleanEpochDiagnosticAxis", 1
        )[1].split("constexpr bool ExecutorDmaCleanEpochWouldSkip", 1)[0]
        for required in (
            "run-sm8650-a750-bb-dma-clean-epoch-diag",
            "run-sm8850-a840-bb-dma-clean-epoch-diag",
            'MemoryPatcher::g_game_serial != "CUSA03173"',
            "profile.exact_mesa_turnip_adreno750",
            "profile.exact_mesa_turnip_adreno840",
            "MobileDriverBuild::MesaTurnipA750Da14d65e44Ib32RolloverBoStats",
        ):
            self.assertIn(required, gate)
        self.assertNotIn("exact_mesa_turnip_a750_d9_lineage", gate)
        self.assertNotIn("exact_proprietary", gate)

    def test_diagnostic_never_skips_the_existing_dma_sweep(self) -> None:
        bind = RASTERIZER.split("bool Rasterizer::BindResources", 1)[1].split(
            "bool Rasterizer::IsComputeMetaClear", 1
        )[0]
        dma = bind.split("if (uses_dma) {", 1)[1].split("\n    return true;", 1)[0]
        sync = "buffer_cache.SynchronizeBuffersInRange("
        self.assertIn(sync, dma)
        self.assertIn("finish_dma_clean_epoch_diagnostic(false);", dma)
        self.assertIn("fault_process_pending = true;", dma)
        self.assertIn("finish_dma_clean_epoch_diagnostic(true);", dma)
        self.assertLess(dma.index(sync), dma.index("fault_process_pending = true;"))
        self.assertLess(
            dma.index("fault_process_pending = true;"),
            dma.index("finish_dma_clean_epoch_diagnostic(true);"),
        )
        candidate = dma.split(
            "dma_clean_epoch_would_skip_candidate =", 1
        )[1].split("const auto finish_dma_clean_epoch_diagnostic", 1)[0]
        self.assertNotIn("return", candidate)
        self.assertNotIn("continue", candidate)

    def test_policy_has_compile_time_negative_contracts(self) -> None:
        policy = RASTERIZER.split(
            "constexpr bool ExecutorDmaCleanEpochWouldSkip", 1
        )[1].split("static bool ExecutorNeedsQualcommPostBindDynamicState", 1)[0]
        for required in (
            "baseline_valid && !prior_sync_failed && !pending_transfers",
            "!fex_writer_activity",
            "cpu_publication_serial == baseline_cpu_publication_serial",
            "mapping_serial == baseline_mapping_serial",
            "topology_serial == baseline_topology_serial",
            "transfer_serial == baseline_transfer_serial",
            "static_assert(ExecutorDmaCleanEpochWouldSkip",
            "static_assert(!ExecutorDmaCleanEpochWouldSkip",
        ):
            self.assertIn(required, policy)

    def test_all_reject_counters_and_serial_observations_are_logged(self) -> None:
        for required in (
            "attempts=%llu",
            "wouldSkip=%llu",
            "rejectNoBaseline=%llu",
            "rejectCpuPublicationChanged=%llu",
            "rejectMappingChanged=%llu",
            "rejectTopologyChanged=%llu",
            "rejectTransferSerialChanged=%llu",
            "rejectPendingTransfers=%llu",
            "rejectFexWriterActivity=%llu",
            "rejectPriorSyncFailed=%llu",
            "cpuSerial=%llu",
            "mappingSerial=%llu",
            "topologySerial=%llu",
            "transferSerial=%llu",
            "fullSyncFailures=%llu",
        ):
            self.assertIn(required, RASTERIZER)
        self.assertIn("u64 next_log_attempt{64};", RASTERIZER_H)
        self.assertIn("diag.next_log_attempt *= 2;", RASTERIZER)

    def test_transfer_epoch_and_fex_writer_activity_block_admission(self) -> None:
        self.assertIn(
            "dma_clean_epoch_transfer_before !=\n"
            "                                           diag.baseline_transfer_serial",
            RASTERIZER,
        )
        self.assertIn(
            "transfer_after == dma_clean_epoch_transfer_before", RASTERIZER
        )
        self.assertIn(
            "GetFexDeferredWriteActivitySnapshot()", RASTERIZER
        )
        self.assertIn("HasActiveWriterOrArmer()", RASTERIZER)

    def test_cpu_publication_and_topology_witnesses_are_conservative(self) -> None:
        self.assertIn("EnableDmaCleanEpochDiagnostic() noexcept", BUFFER_H)
        self.assertIn("GetDmaCleanEpochDiagnosticSerials() const noexcept", BUFFER_H)
        invalidate = BUFFER.split("void BufferCache::InvalidateMemory", 1)[1].split(
            "CpuOwnedWritePublishResult BufferCache::TryPublish", 1
        )[0]
        self.assertLess(
            invalidate.index("RecordDmaCleanEpochCpuPublication();"),
            invalidate.index("memory_tracker->InvalidateRegion"),
        )
        publish = BUFFER.split(
            "CpuOwnedWritePublishResult BufferCache::TryPublish", 1
        )[1].split("void BufferCache::ReadMemory", 1)[0]
        self.assertLess(
            publish.index("RecordDmaCleanEpochCpuPublication();"),
            publish.index("memory_tracker->TryPublishCpuOwnedWriteIfGpuClean"),
        )
        topology = BUFFER.split("void BufferCache::ChangeRegister", 1)[1].split(
            "Buffer& buffer", 1
        )[0]
        self.assertIn("RecordDmaCleanEpochTopologyChange();", topology)

    def test_mapping_witness_covers_map_unmap_and_protection(self) -> None:
        for start, end in (
            ("void Rasterizer::MapMemory", "void Rasterizer::UnmapMemory"),
            ("void Rasterizer::UnmapMemory", "bool Rasterizer::UpdateGuestProtection"),
            (
                "bool Rasterizer::UpdateGuestProtection",
                "bool Rasterizer::BeginGuestProtectionUpdate",
            ),
        ):
            body = RASTERIZER.split(start, 1)[1].split(end, 1)[0]
            self.assertIn("mapping_serial.fetch_add", body)

    def test_android_only_types_leave_native_ps4_ps5_path_clean(self) -> None:
        header_diag = RASTERIZER_H.index(
            "struct ExecutorDmaCleanEpochDiagnosticState"
        )
        header_guard = RASTERIZER_H.rfind("#ifdef __ANDROID__", 0, header_diag)
        header_end = RASTERIZER_H.find("#endif", header_diag)
        self.assertLess(header_guard, header_diag)
        self.assertLess(header_diag, header_end)

        buffer_diag = BUFFER_H.index("struct DmaCleanEpochDiagnosticSerials")
        buffer_guard = BUFFER_H.rfind("#ifdef __ANDROID__", 0, buffer_diag)
        buffer_end = BUFFER_H.find("#endif", buffer_diag)
        self.assertLess(buffer_guard, buffer_diag)
        self.assertLess(buffer_diag, buffer_end)

        gate = RASTERIZER.index(
            "ExecutorBloodborneDmaCleanEpochDiagnosticAxis"
        )
        source_guard = RASTERIZER.rfind("#ifdef __ANDROID__", 0, gate)
        source_end = RASTERIZER.find("#endif", gate)
        self.assertLess(source_guard, gate)
        self.assertLess(gate, source_end)
        self.assertNotIn("run-sm8650-a750-bb-dma-clean-epoch-diag", BUFFER)


if __name__ == "__main__":
    unittest.main()
