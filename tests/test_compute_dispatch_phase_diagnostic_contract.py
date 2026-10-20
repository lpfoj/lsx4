import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
VK = ROOT / "funnel-arm" / "src" / "video_core" / "renderer_vulkan"
RASTERIZER = (VK / "vk_rasterizer.cpp").read_text(encoding="utf-8")
PIPELINE_CACHE = (VK / "vk_pipeline_cache.cpp").read_text(encoding="utf-8")
BUFFER_CACHE = (
    ROOT / "funnel-arm" / "src" / "video_core" / "buffer_cache" / "buffer_cache.cpp"
).read_text(encoding="utf-8")


class ComputeDispatchPhaseDiagnosticContractTests(unittest.TestCase):
    def test_marker_is_cached_explicit_and_shared_by_lookup_and_dispatch(self):
        for source in (RASTERIZER, PIPELINE_CACHE):
            self.assertIn("diag-compute-dispatch-phase", source)
            self.assertIn("static const bool enabled", source)
            self.assertIn("::access", source)
            self.assertNotIn('getenv("diag-compute-dispatch-phase"', source)

        self.assertIn("diag-compute-dispatch-phase", BUFFER_CACHE)
        self.assertIn("static const bool enabled", BUFFER_CACHE)

    def test_lookup_brackets_post_constructor_registration(self):
        body = PIPELINE_CACHE.split(
            "const ComputePipeline* PipelineCache::GetComputePipeline()", 1
        )[1].split("bool PipelineCache::RefreshGraphicsKey", 1)[0]
        ordered = (
            '"before-constructor"',
            "it.value() = std::make_unique<ComputePipeline>",
            '"after-constructor"',
            '"after-record-valid"',
            '"after-register-data"',
            '"after-mark-dirty"',
            '"after-module-link"',
            '"return-valid"',
        )
        positions = []
        cursor = 0
        for token in ordered:
            cursor = body.index(token, cursor)
            positions.append(cursor)
            cursor += len(token)
        self.assertEqual(positions, sorted(positions))

    def test_dispatch_brackets_resource_bind_and_command_emit(self):
        body = RASTERIZER.split("void Rasterizer::DispatchDirect()", 1)[1].split(
            "void Rasterizer::DispatchIndirect", 1
        )[0]
        ordered = (
            '"before-pipeline-lookup"',
            '"after-pipeline-lookup"',
            '"before-bind-resources"',
            '"after-bind-resources"',
            '"after-upload-flush"',
            '"after-end-rendering"',
            '"before-descriptor-bind"',
            '"after-descriptor-bind"',
            '"before-pipeline-bind"',
            '"after-pipeline-bind"',
            '"before-dispatch"',
            '"after-dispatch"',
            '"complete"',
        )
        positions = []
        cursor = 0
        for token in ordered:
            cursor = body.index(token, cursor)
            positions.append(cursor)
            cursor += len(token)
        self.assertEqual(positions, sorted(positions))
        self.assertIn("dispatch_diagnostic_id > 256", body)
        self.assertIn('std::strcmp(phase, "before-bind-resources")', body)
        self.assertIn('std::strcmp(phase, "after-bind-resources")', body)

    def test_resource_binding_diagnostic_splits_special_buffers_textures_and_dma(self):
        body = RASTERIZER.split("bool Rasterizer::BindResources(", 1)[1].split(
            "bool Rasterizer::IsComputeMetaClear", 1
        )[0]
        ordered = (
            '"entry"',
            '"after-image-copy-check"',
            '"after-meta-clear-check"',
            '"after-image-clear-check"',
            '"after-binding-reset"',
            '"after-user-data"',
            '"before-push-ud"',
            '"after-push-ud"',
            '"before-bind-buffers"',
            '"after-bind-buffers"',
            '"before-bind-textures"',
            '"after-bind-textures"',
            '"before-snapshot-guard"',
            "memory->AcquireSnapshotReadGuard()",
            '"after-snapshot-guard"',
            '"before-mapped-ranges-lock"',
            '"after-mapped-ranges-lock"',
            '"before-dma-sync-sweep"',
            '"after-dma-sync-sweep"',
            '"complete"',
        )
        positions = []
        cursor = 0
        for token in ordered:
            cursor = body.index(token, cursor)
            positions.append(cursor)
            cursor += len(token)
        self.assertEqual(positions, sorted(positions))
        self.assertIn("[EXECUTOR_COMPUTE_RESOURCE_PHASE]", body)
        self.assertIn("compute_diagnostic_id != 0", body)
        self.assertIn("compute_diagnostic_id <= 256", body)

    def test_dma_sweep_obeys_memory_topology_lock_order(self):
        body = RASTERIZER.split("bool Rasterizer::BindResources(", 1)[1].split(
            "bool Rasterizer::IsComputeMetaClear", 1
        )[0]
        snapshot = body.index("auto dma_snapshot_guard = memory->AcquireSnapshotReadGuard()")
        mapped_lock = body.index("std::shared_lock lock{mapped_ranges_mutex}")
        mapped_unlock = body.index('log_compute_resource_phase("after-mapped-ranges-lock")')
        synchronize = body.index("buffer_cache.SynchronizeBuffersInRange(")
        self.assertLess(snapshot, mapped_lock)
        self.assertLess(mapped_lock, mapped_unlock)
        self.assertLess(mapped_unlock, synchronize)
        self.assertIn("range_address, range_size, &dma_snapshot_guard", body)

        range_body = BUFFER_CACHE.split(
            "bool BufferCache::SynchronizeBuffersInRange(", 1
        )[1].split("void BufferCache::WriteDataBuffer", 1)[0]
        self.assertIn("external_snapshot_guard", range_body)
        self.assertIn(
            "false, external_snapshot_guard,\n                              compute_diagnostic_id",
            range_body,
        )

    def test_dma_diagnostic_brackets_range_flush_buffer_walk_and_snapshot(self):
        range_body = BUFFER_CACHE.split(
            "bool BufferCache::SynchronizeBuffersInRange(", 1
        )[1].split("void BufferCache::WriteDataBuffer", 1)[0]
        range_ordered = (
            '"range-entry"',
            '"before-transfer-flush"',
            "scheduler.FlushPendingBufferTransfersForRange",
            '"after-transfer-flush"',
            '"before-buffer-walk"',
            '"before-buffer-sync"',
            "SynchronizeBuffer(buffer",
            '"range-complete-ready"',
        )
        positions = [range_body.index(token) for token in range_ordered]
        self.assertEqual(positions, sorted(positions))

        sync_body = BUFFER_CACHE.split(
            "BufferCache::BufferSyncResult BufferCache::SynchronizeBuffer(", 1
        )[1].split("vk::Buffer BufferCache::UploadCopies", 1)[0]
        sync_ordered = (
            '"sync-entry"',
            '"before-snapshot-dependencies"',
            '"after-snapshot-dependencies"',
            '"source-upload-needed"',
            '"transaction-attempt"',
            '"before-snapshot-write-plan"',
            '"after-snapshot-write-plan"',
            '"before-upload-ranges"',
            '"after-upload-ranges"',
            '"sync-complete"',
        )
        positions = [sync_body.index(token) for token in sync_ordered]
        self.assertEqual(positions, sorted(positions))
        self.assertIn("[EXECUTOR_COMPUTE_DMA_SYNC_PHASE]", sync_body)
        self.assertIn("compute_diagnostic_range == 65", sync_body)


if __name__ == "__main__":
    unittest.main()
