import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
FUNNEL_SRC = ROOT / "funnel-arm" / "src"
FUNNEL = FUNNEL_SRC / "video_core"


class DescriptorSnapshotLifetimeContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.buffer_h = (FUNNEL / "buffer_cache" / "buffer.h").read_text(encoding="utf-8")
        cls.buffer_cpp = (FUNNEL / "buffer_cache" / "buffer.cpp").read_text(encoding="utf-8")
        cls.cache_h = (FUNNEL / "buffer_cache" / "buffer_cache.h").read_text(encoding="utf-8")
        cls.cache_cpp = (FUNNEL / "buffer_cache" / "buffer_cache.cpp").read_text(encoding="utf-8")
        cls.region_manager = (
            FUNNEL / "buffer_cache" / "region_manager.h"
        ).read_text(encoding="utf-8")
        cls.memory_tracker = (
            FUNNEL / "buffer_cache" / "memory_tracker.h"
        ).read_text(encoding="utf-8")
        cls.page_manager_h = (FUNNEL / "page_manager.h").read_text(encoding="utf-8")
        cls.page_manager_cpp = (FUNNEL / "page_manager.cpp").read_text(encoding="utf-8")
        cls.memory_h = (FUNNEL_SRC / "core" / "memory.h").read_text(encoding="utf-8")
        cls.memory_cpp = (FUNNEL_SRC / "core" / "memory.cpp").read_text(encoding="utf-8")
        cls.rasterizer = (
            FUNNEL / "renderer_vulkan" / "vk_rasterizer.cpp"
        ).read_text(encoding="utf-8")
        cls.texture_cache_cpp = (
            FUNNEL / "texture_cache" / "texture_cache.cpp"
        ).read_text(encoding="utf-8")
        cls.hle = (
            FUNNEL / "renderer_vulkan" / "vk_shader_hle.cpp"
        ).read_text(encoding="utf-8")

    def test_descriptor_arena_is_lazy_best_effort_and_separate(self):
        self.assertIn("std::unique_ptr<StreamBuffer> descriptor_snapshot_buffer", self.cache_h)
        self.assertIn("new (std::nothrow) StreamBuffer(std::nothrow", self.cache_cpp)
        self.assertIn("snapshot_buffer->MapLeased", self.cache_cpp)
        self.assertNotIn("descriptor_snapshot_buffer{instance, scheduler", self.cache_cpp)

    def test_wrap_is_transactional_before_watch_state_mutation(self):
        map_body = self.buffer_cpp.split("StreamBuffer::Map(u64", 1)[1].split(
            "StreamBuffer::MapLeased", 1
        )[0]
        self.assertLess(map_body.index("PreflightWrap"), map_body.index("invalidation_mark ="))
        self.assertLess(map_body.index("PreflightWrap"), map_body.index("std::swap"))
        self.assertLess(map_body.index("WaitPendingOperations"), map_body.index("mapped_size = size"))

    def test_descriptor_batches_have_exact_consuming_tick_lifecycle(self):
        for token in (
            "BeginReadOnlyDescriptorSnapshotBatch",
            "RetireReadOnlyDescriptorSnapshotBatch",
            "CancelReadOnlyDescriptorSnapshotBatch",
            "SCOPE_EXIT { buffer_cache.CancelReadOnlyDescriptorSnapshotBatch(); }",
        ):
            self.assertIn(token, self.rasterizer + self.cache_cpp)
        self.assertGreaterEqual(
            self.rasterizer.count("RetireReadOnlyDescriptorSnapshotBatch(scheduler.CurrentTick())"),
            4,
        )

    def test_indirect_and_hle_sources_use_tick_retired_snapshots(self):
        self.assertIn("ReadOnlyBufferUsage::Indirect", self.rasterizer)
        self.assertIn("ReadOnlyBufferUsage::HleTransfer", self.hle)
        gate = self.cache_cpp.split("ExecutorReadOnlyRenameUsageEnabled", 1)[1].split(
            "ExecutorShouldLogBufferReadbackOrdinal", 1
        )[0]
        self.assertIn("run-bisect-disable-read-only-rename-indirect", gate)
        self.assertIn("run-bisect-disable-read-only-rename-hle", gate)
        self.assertNotIn(
            "usage == BufferCache::ReadOnlyBufferUsage::Indirect ||", gate
        )

        obtain = self.cache_cpp.split("BufferCache::ObtainReadOnlyBuffer", 1)[1].split(
            "BufferCache::ObtainBufferForImage", 1
        )[0]
        alignment = obtain.split("const u64 alignment", 1)[1].split(
            "const bool descriptor_batch_was_empty", 1
        )[0]
        self.assertIn("case ReadOnlyBufferUsage::Indirect:", alignment)
        self.assertIn("case ReadOnlyBufferUsage::HleTransfer:", alignment)
        self.assertIn("return u64{4};", alignment)
        self.assertIn("snapshot_buffer->RetainCurrentAllocation();", obtain)
        self.assertIn("scheduler.QueueBufferCopies(", self.hle)
        self.assertIn("cmdbuf.drawIndirect", self.rasterizer)
        self.assertIn("cmdbuf.dispatchIndirect", self.rasterizer)

    def test_ordinary_stream_buffers_do_not_allocate_descriptor_ledger(self):
        ordinary_ctor = self.buffer_cpp.split("StreamBuffer::StreamBuffer(const", 1)[1].split(
            "StreamBuffer::StreamBuffer(std::nothrow_t", 1
        )[0]
        best_effort_ctor = self.buffer_cpp.split(
            "StreamBuffer::StreamBuffer(std::nothrow_t", 1
        )[1].split("StreamBuffer::Map(u64", 1)[0]
        self.assertNotIn("lease_blocks.resize", ordinary_ctor)
        self.assertIn("lease_blocks.resize", best_effort_ctor)

    def test_snapshot_staleness_is_an_exact_region_bit_contract(self):
        self.assertIn("RegionBits snapshot_stale;", self.region_manager)
        self.assertIn("snapshot_stale.Clear();", self.region_manager)
        self.assertNotIn("descriptor_snapshot_stale_ranges", self.cache_h + self.cache_cpp)

        mark_body = self.region_manager.split("void MarkSnapshotStale", 1)[1].split(
            "void ClearSnapshotStale", 1
        )[0]
        clear_body = self.region_manager.split("void ClearSnapshotStale", 1)[1].split(
            "bool IsRegionSnapshotStale", 1
        )[0]
        query_body = self.region_manager.split("bool IsRegionSnapshotStale", 1)[1].split(
            "void ForEachUploadRange", 1
        )[0]
        self.assertIn("snapshot_stale.SetRange(start_page, end_page)", mark_body)
        self.assertIn("snapshot_stale.UnsetRange(start_page, end_page)", clear_body)
        self.assertIn("snapshot_stale.AnyInRange(start_page, end_page)", query_body)

        upload_body = self.region_manager.split("void ForEachUploadRange(", 1)[1].split(
            "template <Type type>", 1
        )[0]
        ordered_tokens = (
            "RegionBits cpu_mask(cpu, start_page, end_page)",
            "RegionBits upload_mask(snapshot_stale, start_page, end_page)",
            "upload_mask |= cpu_mask",
            "snapshot_stale |= cpu_mask",
            "cpu.UnsetRange(start_page, end_page)",
            "for (const auto& [start, end] : upload_mask)",
        )
        for before, after in zip(ordered_tokens, ordered_tokens[1:]):
            self.assertLess(upload_body.index(before), upload_body.index(after))

    def test_snapshot_dirty_bits_clear_only_after_a_successful_stable_upload(self):
        upload_body = self.memory_tracker.split(
            "SnapshotTransactionResult ForEachUploadRange", 1
        )[1].split(
            "template <typename SnapshotFunc>", 1
        )[0]
        self.assertLess(
            upload_body.index("const bool upload_succeeded = on_upload()"),
            upload_body.index("if (upload_succeeded)"),
        )
        success_body = upload_body.split("if (upload_succeeded)", 1)[1].split(
            "// Direct-VA fallback", 1
        )[0]
        self.assertIn("slice.manager->ClearSnapshotStale", success_body)
        self.assertIn("for (const Slice& slice : slices)", success_body)
        self.assertNotIn("snapshot_slices", success_body)
        self.assertLess(
            upload_body.index("if (!finish_descriptor_snapshot())"),
            upload_body.index("if (upload_succeeded)"),
        )
        self.assertLess(
            upload_body.index("if (!finish_descriptor_snapshot())"),
            upload_body.rindex("publish_gpu_ownership()"),
        )

        snapshot_body = self.memory_tracker.split(
            "SnapshotTransactionResult WithCpuSnapshot", 1
        )[1].split(
            "template <bool clear>", 1
        )[0]
        ordered_tokens = (
            "manager->lock.lock()",
            "TryTrackDescriptorSnapshotWrite",
            "ForEachModifiedRange<Type::CPU, true>",
            "manager->MarkSnapshotStale",
            "const bool copy_succeeded = snapshot()",
        )
        for before, after in zip(ordered_tokens, ordered_tokens[1:]):
            self.assertLess(snapshot_body.index(before), snapshot_body.index(after))
        self.assertLess(
            snapshot_body.index("std::ranges::sort(locked_managers"),
            snapshot_body.index("std::ranges::unique(locked_managers)"),
        )
        self.assertLess(
            snapshot_body.index("std::ranges::unique(locked_managers)"),
            snapshot_body.index("manager->lock.lock()"),
        )
        self.assertIn(
            "for (auto it = locked_managers.rbegin(); it != locked_managers.rend(); ++it)",
            snapshot_body,
        )

        alias_dirty_tokens = (
            "std::ranges::any_of(snapshot_slices, snapshot_dirty)",
            "for (const Slice& slice : snapshot_slices)",
            "ForEachModifiedRange<Type::CPU, true>",
            "manager->MarkSnapshotStale(dirty_addr, dirty_size)",
            "if (alias_source_dirty)",
            "for (const Slice& slice : slices)",
            "const bool copy_succeeded = snapshot()",
        )
        for token in alias_dirty_tokens:
            self.assertIn(token, snapshot_body)

    def test_successful_upload_keeps_sibling_alias_b_stale(self):
        upload_body = self.memory_tracker.split(
            "SnapshotTransactionResult ForEachUploadRange", 1
        )[1].split("template <typename SnapshotFunc>", 1)[0]
        self.assertIn(
            "for (const auto& [range_addr, range_size] : snapshot_alias_ranges)",
            upload_body,
        )
        self.assertIn("snapshot_slices.push_back", upload_body)

        success_body = upload_body.split("if (upload_succeeded)", 1)[1].split(
            "// Direct-VA fallback", 1
        )[0]
        self.assertIn("for (const Slice& slice : slices)", success_body)
        self.assertEqual(success_body.count("ClearSnapshotStale"), 1)
        self.assertNotIn("snapshot_slices", success_body)

        resolve_body = self.cache_cpp.split(
            "bool BufferCache::ResolveSnapshotDependencies", 1
        )[1].split("BufferCache::BufferSyncResult BufferCache::SynchronizeBuffer", 1)[0]
        self.assertIn(
            "snapshot_plan.AliasRanges()",
            resolve_body,
        )

    def test_snapshot_alias_plan_is_generation_cached_merged_and_fail_closed(self):
        for token in (
            "struct WritableAliasRecord",
            "std::mutex alias_cache_mutex",
            "u64 alias_cache_generation",
            "bool writable_alias_cache_valid",
            "std::vector<WritableAliasRecord> writable_alias_cache",
            "std::vector<PAddr> writable_alias_prefix_max_end",
            "std::atomic<u64> vma_generation",
        ):
            self.assertIn(token, self.memory_h)

        rebuild_body = self.memory_cpp.split(
            "bool MemoryManager::RebuildWritableAliasCacheLocked", 1
        )[1].split("bool MemoryManager::BuildSnapshotWriteRangesUncached", 1)[0]
        self.assertIn(".cpu_writable = True(vma.prot & MemoryProt::CpuWrite)", rebuild_body)
        self.assertIn("writable_alias_cache.push_back", rebuild_body)
        self.assertIn("std::ranges::sort(writable_alias_cache", rebuild_body)
        self.assertIn("writable_alias_prefix_max_end.push_back", rebuild_body)
        self.assertIn("alias_cache_generation = vma_generation.load", rebuild_body)

        build_body = self.memory_cpp.split(
            "bool MemoryManager::BuildSnapshotWriteRangesUncached", 1
        )[1].split("bool MemoryManager::BuildSnapshotWriteRanges(", 1)[0]
        ordered_tokens = (
            "ASSERT(guard.owner == this)",
            "std::unique_lock alias_lk{alias_cache_mutex}",
            "alias_cache_generation != vma_generation.load",
            "RebuildWritableAliasCacheLocked()",
            "if (!writable_alias_cache_valid)",
            "std::upper_bound(writable_alias_prefix_max_end.begin()",
            "const PAddr overlap_begin",
            "append_alias_range(alias_begin",
            "if (alias.cpu_writable)",
            "append_write_range(alias_begin",
            "normalize_ranges(write_ranges)",
            "normalize_ranges(alias_ranges)",
        )
        for token in ordered_tokens:
            self.assertIn(token, build_body)
        for before, after in zip(ordered_tokens, ordered_tokens[1:]):
            self.assertLess(build_body.index(before), build_body.index(after))
        invalid_cache_path = build_body.split("if (!writable_alias_cache_valid)", 1)[1].split(
            "const auto append_write_range", 1
        )[0]
        self.assertIn("return false", invalid_cache_path)
        self.assertIn("write_ranges.clear()", build_body)
        self.assertIn("alias_ranges.clear()", build_body)
        file_mapping_path = build_body.split("if (vma.type == VMAType::File)", 1)[1].split(
            "const u64 offset_in_vma", 1
        )[0]
        self.assertIn("return false", file_mapping_path)

        self.assertIn("static constexpr u32 CACHING_PAGEBITS = 14", self.cache_h)
        obtain_body = self.cache_cpp.split("BufferCache::ObtainReadOnlyBuffer", 1)[1].split(
            "BufferCache::ObtainBufferForImage", 1
        )[0]
        self.assertIn("MaxRenameRangeBytes = CACHING_PAGESIZE", obtain_body)
        self.assertLess(
            obtain_body.index("size > MaxRenameRangeBytes"),
            obtain_body.index("snapshot_buffer->MapLeased"),
        )

    def test_snapshot_transaction_matrix_is_typed_and_fail_closed(self):
        result_enum = self.memory_tracker.split(
            "enum class SnapshotTransactionResult", 1
        )[1].split("};", 1)[0]
        for result in (
            "Complete", "NoUpload", "GpuDirty", "Busy", "Overlap",
            "Unsupported", "Invalid", "CopyFailed",
        ):
            self.assertIn(result, result_enum)

        action_body = self.memory_tracker.split(
            "constexpr SnapshotTransactionAction DecideSnapshotTransactionAction", 1
        )[1].split("constexpr SnapshotTransactionResult SnapshotTrackFailureResult", 1)[0]
        complete_cases = action_body.split("case SnapshotTransactionResult::Complete", 1)[1].split(
            "case SnapshotTransactionResult::GpuDirty", 1
        )[0]
        self.assertIn("case SnapshotTransactionResult::NoUpload", complete_cases)
        self.assertIn("return SnapshotTransactionAction::Complete", complete_cases)
        gpu_case = action_body.split("case SnapshotTransactionResult::GpuDirty", 1)[1].split(
            "case SnapshotTransactionResult::Busy", 1
        )[0]
        self.assertIn("return SnapshotTransactionAction::ResolveGpu", gpu_case)
        retry_cases = action_body.split("case SnapshotTransactionResult::Busy", 1)[1].split(
            "case SnapshotTransactionResult::Unsupported", 1
        )[0]
        self.assertIn("case SnapshotTransactionResult::Overlap", retry_cases)
        self.assertIn("return SnapshotTransactionAction::Retry", retry_cases)
        fail_closed_cases = action_body.split(
            "case SnapshotTransactionResult::Unsupported", 1
        )[1]
        self.assertIn("case SnapshotTransactionResult::Invalid", fail_closed_cases)
        self.assertIn("case SnapshotTransactionResult::CopyFailed", fail_closed_cases)
        self.assertIn("return SnapshotTransactionAction::FailClosed", fail_closed_cases)

        track_matrix = self.memory_tracker.split(
            "constexpr SnapshotTransactionResult SnapshotTrackFailureResult", 1
        )[1].split("class MemoryTracker", 1)[0]
        for track_result in ("Unsupported", "Invalid"):
            case_body = track_matrix.split(
                f"case DescriptorSnapshotTrackResult::{track_result}", 1
            )[1].split("return", 1)[1].split(";", 1)[0]
            self.assertIn(f"SnapshotTransactionResult::{track_result}", case_body)

    def test_bulk_writer_count_is_exact_until_sticky_fail_closed_overflow(self):
        self.assertIn("u32 num_bulk_writers : 13;", self.page_manager_cpp)
        self.assertIn("u32 bulk_writer_overflow : 1;", self.page_manager_cpp)
        active_body = self.page_manager_cpp.split("bool BulkWriteActive", 1)[1].split(
            "u8 UpdateBulkWriterCount", 1
        )[0]
        self.assertIn(
            "state->num_bulk_writers != 0 || state->bulk_writer_overflow != 0",
            active_body,
        )

        count_body = self.page_manager_cpp.split("u8 UpdateBulkWriterCount", 1)[1].split(
            "void SetWriteFaultBypass", 1
        )[0]
        self.assertEqual(count_body.count("if (state.bulk_writer_overflow != 0)"), 2)
        self.assertIn("if (state.num_bulk_writers == 8191)", count_body)
        self.assertIn("state.bulk_writer_overflow = 1", count_body)
        self.assertNotIn("state.bulk_writer_overflow = 0", count_body)
        self.assertIn("if (state.num_bulk_writers == 0)", count_body)
        self.assertIn('ASSERT_MSG(false, "Bulk writer count underflow")', count_body)

        activity_body = self.page_manager_cpp.split("void SetBulkGuestWriteActivity", 1)[1].split(
            "static bool GuestFaultSignalHandler", 1
        )[0]
        self.assertLess(
            activity_body.index("const u8 writer_count = UpdateBulkWriterCount"),
            activity_body.index("if (writer_count != 1)"),
        )
        self.assertLess(
            activity_body.index("if (writer_count != 1)"),
            activity_body.index("SetWriteFaultBypass(page, true)"),
        )
        self.assertLess(
            activity_body.index("writer_count == 0"),
            activity_body.index("SetWriteFaultBypass(page, false)"),
        )

    def test_descriptor_tracking_is_typed_fail_closed_and_never_yields(self):
        for result in ("Tracked", "Busy", "Overlap", "Unsupported", "Invalid"):
            self.assertIn(result, self.page_manager_h.split("enum class DescriptorSnapshotTrackResult", 1)[1].split("};", 1)[0])

        track_body = self.page_manager_cpp.split(
            "DescriptorSnapshotTrackResult TryTrackDescriptorSnapshotWrite", 1
        )[1].split("bool ConsumeDescriptorSnapshotWriteFault", 1)[0]
        self.assertNotIn("yield", track_body)
        self.assertNotIn("sleep", track_body)
        self.assertNotIn("Wait(", track_body)
        self.assertIn("BulkWriteActive(probe) || WriteFaultBypassed(probe)", track_body)
        self.assertIn("return DescriptorSnapshotTrackResult::Busy", track_body)
        self.assertIn("return DescriptorSnapshotTrackResult::Overlap", track_body)
        self.assertIn("return DescriptorSnapshotTrackResult::Tracked", track_body)

        snapshot_body = self.memory_tracker.split(
            "SnapshotTransactionResult WithCpuSnapshot", 1
        )[1].split(
            "template <bool clear>", 1
        )[0]
        self.assertIn(
            "if (track_result != DescriptorSnapshotTrackResult::Tracked)", snapshot_body
        )
        self.assertIn("return SnapshotTrackFailureResult(track_result)", snapshot_body)

        upload_body = self.memory_tracker.split(
            "SnapshotTransactionResult ForEachUploadRange", 1
        )[1].split("template <typename SnapshotFunc>", 1)[0]
        self.assertIn("return SnapshotTrackFailureResult(track_result)", upload_body)

    def test_descriptor_fault_conflicts_retry_and_never_reports_false_progress(self):
        self.assertIn("descriptor_snapshot_conflict", self.page_manager_cpp)
        consume_body = self.page_manager_cpp.split(
            "bool ConsumeDescriptorSnapshotWriteFault", 1
        )[1].split("bool UntrackDescriptorSnapshotWrite", 1)[0]
        self.assertLess(
            consume_body.index("state.descriptor_snapshot_write_watcher = 0"),
            consume_body.index("SetDescriptorSnapshotConflict(page, true)"),
        )
        self.assertIn("Protect(", consume_body)

        untrack_body = self.page_manager_cpp.split(
            "bool UntrackDescriptorSnapshotWrite", 1
        )[1].split("template <bool track>", 1)[0]
        self.assertIn("DescriptorSnapshotConflicted(page)", untrack_body)
        self.assertIn("return clean", untrack_body)

        handler_body = self.page_manager_cpp.split(
            "static bool GuestFaultSignalHandler", 1
        )[1].split("~Impl()", 1)[0]
        self.assertIn("ConsumeDescriptorSnapshotWriteFault", handler_body)
        self.assertIn(
            "owner->FaultAccessResolved(addr, is_write, is_execute,",
            handler_body,
        )
        handled_expression = handler_body.split("const bool handled =", 1)[1].split(";", 1)[0]
        self.assertNotIn("cache_event", handled_expression)

        classify_body = self.page_manager_cpp.split(
            "FaultOwnership ClassifyAndRepairFault", 1
        )[1].split("bool FaultAccessResolved", 1)[0]
        self.assertLess(
            classify_body.index("if (!baseline_allowed)"),
            classify_body.index("Protect("),
        )
        self.assertIn("return FaultOwnership::Unowned", classify_body)
        self.assertIn("EffectivePerms(page, state.WatcherPerms())", classify_body)
        self.assertNotIn("FillActualWriteProtection", classify_body)
        self.assertNotIn("FillActualReadProtection", classify_body)

        self.assertIn(
            "std::array<u32, PAGES_PER_LOCK> fault_generation{};",
            self.page_manager_cpp,
        )
        self.assertIn("u16& generation", classify_body)
        resolved_body = self.page_manager_cpp.split(
            "bool FaultAccessResolved", 1
        )[1].split("void ReleaseDeferredWriteProtectionRange", 1)[0]
        self.assertIn("FaultGeneration(page, is_write) != captured_generation", resolved_body)
        self.assertLess(
            resolved_body.index("if (!baseline_allowed)"),
            resolved_body.index("completed_generation"),
        )
        jit_consume_body = self.page_manager_cpp.split(
            "bool ConsumeJitPollWriteFault", 1
        )[1].split("void SetJitPollWritePublishCallback", 1)[0]
        self.assertIn("AdvanceFaultGeneration(page, true)", jit_consume_body)

    def test_unregistered_buffer_fault_reconciles_tracker_ownership(self):
        invalidate_body = self.cache_cpp.split(
            "void BufferCache::InvalidateMemory", 1
        )[1].split("void BufferCache::ReadMemory", 1)[0]
        unregistered_path = invalidate_body.split(
            "if (!IsRegionRegistered(device_addr, size))", 1
        )[1].split("liverpool->NotifyMemoryPublicationForExecutor", 1)[0]
        self.assertIn(
            "memory_tracker->MarkRegionAsCpuModified(device_addr, size)",
            unregistered_path,
        )
        self.assertLess(
            unregistered_path.index("MarkRegionAsCpuModified"),
            unregistered_path.index("\n        return;"),
        )

    def test_buffer_snapshot_dependency_excludes_primary_and_preserves_no_upload_fast_path(self):
        dependency_body = self.cache_cpp.split(
            "bool BufferCache::ResolveSnapshotDependencies", 1
        )[1].split("BufferCache::BufferSyncResult BufferCache::SynchronizeBuffer", 1)[0]
        self.assertIn("dependency_alias_ranges", dependency_body)
        self.assertIn("snapshot_plan.SiblingAliasRanges()", dependency_body)
        self.assertIn(
            "for (const auto& [range_addr, range_size] : dependency_alias_ranges)",
            dependency_body,
        )
        self.assertIn(
            "dependency_alias_ranges, [&](const auto& range)",
            dependency_body,
        )
        publication_tail = dependency_body.split("if (backing_was_published", 1)[1]
        self.assertIn(
            "snapshot_plan.AliasRanges()",
            publication_tail,
        )

        sync_body = self.cache_cpp.split(
            "BufferCache::BufferSyncResult BufferCache::SynchronizeBuffer", 1
        )[1].split("vk::Buffer BufferCache::UploadCopies", 1)[0]
        self.assertIn(
            "ownership_transaction_needed ? SnapshotTransactionResult::Invalid",
            sync_body,
        )
        self.assertIn(
            ": SnapshotTransactionResult::NoUpload",
            sync_body,
        )
        self.assertIn("ownership_transaction_needed", sync_body)
        ownership_gate = sync_body.split(
            "const bool ownership_transaction_needed", 1
        )[1].split("SnapshotTransactionResult transaction_result", 1)[0]
        self.assertIn("upload_needed", ownership_gate)
        self.assertIn("is_written", ownership_gate)
        self.assertIn("memory_tracker->IsRegionGpuModified", ownership_gate)
        self.assertIn("if (!ownership_transaction_needed)", sync_body)
        self.assertNotIn(
            "memory_tracker->MarkRegionAsGpuModified(device_addr, size)",
            sync_body,
        )

    def test_primary_upload_gpu_conflict_is_page_granular_and_alias_query_stays_broad(self):
        overlap_body = self.region_manager.split(
            "bool HasGpuDirtyUploadOverlap", 1
        )[1].split("LockType lock", 1)[0]
        ordered_tokens = (
            "RegionBits upload_mask(cpu, start_page, end_page)",
            "RegionBits stale_mask(snapshot_stale, start_page, end_page)",
            "upload_mask |= stale_mask",
            "RegionBits gpu_mask(gpu, start_page, end_page)",
            "upload_mask &= gpu_mask",
            "return upload_mask.Any()",
        )
        for before, after in zip(ordered_tokens, ordered_tokens[1:]):
            self.assertLess(overlap_body.index(before), overlap_body.index(after))

        upload_body = self.memory_tracker.split(
            "SnapshotTransactionResult ForEachUploadRange", 1
        )[1].split("template <typename SnapshotFunc>", 1)[0]
        conflict_body = upload_body.split("const auto gpu_dirty", 1)[1].split(
            "for (const auto& [range_addr, range_size] : snapshot_write_ranges)", 1
        )[0]
        self.assertIn(
            "slice.manager->HasGpuDirtyUploadOverlap(slice.offset,",
            conflict_body,
        )
        self.assertIn(
            "std::ranges::any_of(slices, upload_overlaps_gpu)",
            conflict_body,
        )
        self.assertIn(
            "std::ranges::any_of(snapshot_slices, gpu_dirty)",
            conflict_body,
        )
        self.assertNotIn("std::ranges::any_of(slices, gpu_dirty)", conflict_body)
        self.assertNotIn(
            "std::ranges::any_of(snapshot_slices, upload_overlaps_gpu)",
            conflict_body,
        )

    def test_buffer_sync_excludes_primary_from_initial_and_transaction_alias_ranges(self):
        plan_body = self.memory_cpp.split(
            "bool MemoryManager::BuildSnapshotWritePlan", 1
        )[1].split("bool MemoryManager::IsWritableMapping", 1)[0]
        for token in (
            "const VAddr request_end = virtual_addr + size",
            "if (range_addr < virtual_addr)",
            "left_end - range_addr",
            "if (request_end < range_end)",
            "range_end - right_begin",
            "owned_sibling_alias_ranges.emplace_back",
        ):
            self.assertIn(token, plan_body)

        sync_body = self.cache_cpp.split(
            "BufferCache::BufferSyncResult BufferCache::SynchronizeBuffer", 1
        )[1].split("vk::Buffer BufferCache::UploadCopies", 1)[0]
        initial_body = sync_body.split(
            "Core::MemoryManager::SnapshotWritePlan initial_snapshot_plan", 1
        )[1].split("const bool upload_needed", 1)[0]
        initial_tokens = (
            "memory->BuildSnapshotSiblingPlan",
            "initial_snapshot_plan.SiblingAliasRanges()",
            "memory_tracker->AreSnapshotRangesDirty",
        )
        for token in initial_tokens:
            self.assertIn(token, initial_body)
        self.assertLess(initial_body.index(initial_tokens[0]),
                        initial_body.index("memory_tracker->AreSnapshotRangesDirty"))

        transaction_body = sync_body.split(
            "for (u32 attempt = 0; attempt < MaxSnapshotTransactionAttempts", 1
        )[1].split("const auto action = DecideSnapshotTransactionAction", 1)[0]
        transaction_tokens = (
            "memory->BuildSnapshotWritePlan",
            "snapshot_plan.SiblingAliasRanges()",
            "memory_tracker->ForEachUploadRange",
        )
        for before, after in zip(transaction_tokens, transaction_tokens[1:]):
            self.assertLess(transaction_body.index(before), transaction_body.index(after))

    def test_demand_readback_windows_are_discovered_before_empty_wave_exit(self):
        wave_body = self.cache_cpp.split(
            "void BufferCache::DownloadBufferMemoryWave", 1
        )[1].split("template <bool async>", 1)[0]
        primary_queue = wave_body.index(
            "queue_buffer_ranges(primary_buffer, window_addr, window_size, true)"
        )
        empty_guard = wave_body.index("if (copies.empty())", primary_queue)
        recent_window = wave_body.index("const auto queue_window", primary_queue)
        queue_recent = wave_body.index(
            "queue_window(recent_readback_windows[index])", recent_window
        )
        self.assertLess(primary_queue, empty_guard)
        self.assertLess(recent_window, queue_recent)
        self.assertLess(queue_recent, empty_guard)
        guard_body = wave_body[empty_guard:]
        self.assertIn("MarkRegionAsCpuModified", guard_body)
        self.assertIn("return", guard_body)

        protect_body = self.page_manager_cpp.split(
            "void Protect(VAddr address", 1
        )[1].split("void ReconcileProtectionRange", 1)[0]
        self.assertIn("EffectivePerms(page, perms)", protect_body)
        self.assertNotIn("actual_perms |= Core::MemoryPermission::Write", protect_body)

        self.assertIn("UpdateRasterizerMemoryProtection", self.memory_cpp)
        self.assertIn(
            "if (!UpdateRasterizerMemoryProtection(rasterizer, addr, adjusted_size,",
            self.memory_cpp,
        )

        for body in (
            self.memory_tracker.split(
                "SnapshotTransactionResult ForEachUploadRange", 1
            )[1].split("template <typename SnapshotFunc>", 1)[0],
            self.memory_tracker.split(
                "SnapshotTransactionResult WithCpuSnapshot", 1
            )[1].split("template <bool clear>", 1)[0],
        ):
            self.assertIn("finish_descriptor_snapshot", body)
            self.assertIn("SnapshotTransactionResult::Busy", body)

    def test_unregistered_snapshot_and_copy_failure_use_stable_backing(self):
        obtain_body = self.cache_cpp.split("BufferCache::ObtainReadOnlyBuffer", 1)[1].split(
            "BufferCache::ObtainBufferForImage", 1
        )[0]
        self.assertLess(
            obtain_body.index("buffer_ranges.Contains(device_addr, size)"),
            obtain_body.index("snapshot_buffer->MapLeased"),
        )
        unregistered_gate = obtain_body.split("const bool fully_registered", 1)[1].split(
            "const bool stable_snapshot_stale", 1
        )[0]
        self.assertIn("if (!fully_registered && !replay_memory)", unregistered_gate)
        self.assertIn("fallback(ReadOnlyRenameFallback::Unregistered)", unregistered_gate)

        clean_no_alias = obtain_body.split(
            "if (!snapshot_dirty && !memory->PhysicalAliasingMayExist())", 1
        )[1].split("if (!snapshot_dirty)", 1)[0]
        self.assertIn(
            "return fallback(ReadOnlyRenameFallback::NotCpuDirty)",
            clean_no_alias,
        )
        self.assertNotIn("BuildSnapshotWritePlan", clean_no_alias)

        dependency_body = self.cache_cpp.split(
            "bool BufferCache::ResolveSnapshotDependencies", 1
        )[1].split("BufferCache::BufferSyncResult", 1)[0]
        no_alias_dependency = dependency_body.split(
            "if (!memory->PhysicalAliasingMayExist()", 1
        )[1].split("static constexpr u32 MaxResolveAttempts", 1)[0]
        self.assertIn("return true", no_alias_dependency)

        fallback_body = obtain_body.split("const auto fallback =", 1)[1].split(
            "if (!ExecutorReadOnlyRenameUsageEnabled", 1
        )[0]
        self.assertIn("ObtainBuffer(device_addr, size, false, false", fallback_body)
        copy_failure = obtain_body.split("if (!copy_succeeded)", 1)[1].split(
            "if (descriptor_usage)", 2
        )[1]
        self.assertIn("return fallback(ReadOnlyRenameFallback::CopyFailed)", copy_failure)

    def test_memory_topology_guard_ends_before_vulkan_work(self):
        guard_body = self.memory_h.split("class SnapshotReadGuard", 1)[1].split(
            "explicit MemoryManager", 1
        )[0]
        self.assertIn("topology_lock{owner_.unmap_mutex}", guard_body)
        self.assertIn("lock{owner_.mutex}", guard_body)
        self.assertIn(
            "std::unique_lock<std::mutex> topology_lock",
            guard_body,
        )
        self.assertIn("std::mutex unmap_mutex", self.memory_h)
        self.assertIn("std::shared_lock<Common::SharedFirstMutex> lock", guard_body)

        guarded_copy = self.memory_cpp.split(
            "TryCopyBackingMemory(const SnapshotReadGuard& guard", 1
        )[1].split("TryCopyBackingMemoryLocked", 1)[0]
        self.assertIn("ASSERT(guard.owner == this)", guarded_copy)
        self.assertNotIn("shared_lock", guarded_copy)

        self.assertIn(
            "TryCopyReadableMemory(const SnapshotReadGuard& guard", self.memory_h
        )
        guarded_readable_copy = self.memory_cpp.split(
            "TryCopyReadableMemory(const SnapshotReadGuard& guard", 1
        )[1].split("TryCopyReadableMemoryLocked", 1)[0]
        self.assertIn("ASSERT(guard.owner == this)", guarded_readable_copy)
        self.assertNotIn("shared_lock", guarded_readable_copy)

        obtain_body = self.cache_cpp.split("BufferCache::ObtainReadOnlyBuffer", 1)[1].split(
            "BufferCache::ObtainBufferForImage", 1
        )[0]
        snapshot_scope = obtain_body.split("bool copy_succeeded = false", 1)[1].split(
            "if (!copy_succeeded)", 1
        )[0]
        guard_token = "auto memory_guard = memory->AcquireSnapshotReadGuard()"
        self.assertIn(guard_token, snapshot_scope)
        self.assertEqual(obtain_body.count("PhysicalAliasingMayExist()"), 1)
        final_guard_start = snapshot_scope.rindex(guard_token)
        final_guard_end = snapshot_scope.index(
            "DecideSnapshotTransactionAction(snapshot_result)", final_guard_start
        )
        final_guard_scope = snapshot_scope[final_guard_start:final_guard_end]
        self.assertLess(
            final_guard_scope.index(guard_token),
            final_guard_scope.index("memory->BuildSnapshotWritePlan"),
        )
        self.assertLess(
            final_guard_scope.index("memory->BuildSnapshotWritePlan"),
            final_guard_scope.index("memory_tracker->WithCpuSnapshot"),
        )
        self.assertIn("snapshot_plan.WriteRanges()", final_guard_scope)
        self.assertIn("snapshot_plan.AliasRanges()", final_guard_scope)
        compact_snapshot_scope = " ".join(final_guard_scope.split())
        self.assertIn("TryCopyBackingMemory( memory_guard", compact_snapshot_scope)
        self.assertNotIn("std::this_thread::yield", final_guard_scope)
        for vulkan_work in ("CommitLeased", "QueueBufferUploadCopies", "pipelineBarrier2"):
            self.assertNotIn(vulkan_work, final_guard_scope)
        self.assertLess(
            snapshot_scope.index("DecideSnapshotTransactionAction(snapshot_result)"),
            snapshot_scope.index("std::this_thread::yield()"),
        )
        self.assertIn("MaxSnapshotTransactionAttempts = 64", snapshot_scope)

        resolve_body = self.cache_cpp.split(
            "bool BufferCache::ResolveSnapshotDependencies", 1
        )[1].split("BufferCache::BufferSyncResult BufferCache::SynchronizeBuffer", 1)[0]
        self.assertIn("MaxResolveAttempts = 4", resolve_body)
        self.assertLess(
            resolve_body.index("memory->BuildSnapshotSiblingPlan"),
            resolve_body.index("WaitForPendingReadback"),
        )
        self.assertLess(
            resolve_body.index("WaitForPendingReadback"),
            resolve_body.index("ReadMemory("),
        )

        sync_body = self.cache_cpp.split(
            "BufferCache::BufferSyncResult BufferCache::SynchronizeBuffer", 1
        )[1].split(
            "vk::Buffer BufferCache::UploadCopies", 1
        )[0]
        resolve_call = sync_body.index(
            "ResolveSnapshotDependencies(device_addr, size,"
        )
        transaction_start = sync_body.index(
            "SnapshotTransactionResult transaction_result"
        )
        self.assertLess(
            resolve_call,
            transaction_start,
        )
        self.assertIn(
            "external_snapshot_guard",
            sync_body[resolve_call:transaction_start],
        )
        guard_end = sync_body.index("const auto action = DecideSnapshotTransactionAction")
        guard_start = sync_body.rindex(guard_token, 0, guard_end)
        guarded_upload = sync_body[guard_start:guard_end]
        self.assertLess(
            guarded_upload.index("memory->BuildSnapshotWritePlan"),
            guarded_upload.index("memory_tracker->ForEachUploadRange"),
        )
        self.assertIn("memory_tracker->ForEachUploadRange", guarded_upload)
        compact_guarded_upload = " ".join(guarded_upload.split())
        self.assertIn("TryCopyBackingMemory( memory_guard", compact_guarded_upload)
        self.assertIn("TryCopyReadableMemory( memory_guard", compact_guarded_upload)
        self.assertLess(
            guarded_upload.index("TryCopyBackingMemory"),
            guarded_upload.index("TryCopyReadableMemory"),
        )
        self.assertNotIn("std::this_thread::yield", guarded_upload)
        for vulkan_work in ("staging_buffer.Commit", "QueueBufferUploadCopies", "pipelineBarrier2"):
            self.assertNotIn(vulkan_work, guarded_upload)
        self.assertLess(
            sync_body.index("const auto action = DecideSnapshotTransactionAction"),
            sync_body.index("std::this_thread::yield()"),
        )
        self.assertIn("MaxSnapshotTransactionAttempts = 64", sync_body)

    def test_release_sync_failures_propagate_through_dma_resource_binding(self):
        self.assertIn(
            "[[nodiscard]] bool SynchronizeBuffersInRange", self.cache_h
        )
        range_sync = self.cache_cpp.split(
            "bool BufferCache::SynchronizeBuffersInRange", 1
        )[1].split("void BufferCache::WriteDataBuffer", 1)[0]
        self.assertIn("bool synchronized = true", range_sync)
        self.assertIn("ForEachBufferInRange", range_sync)
        self.assertIn("synchronized &= SynchronizeBuffer", range_sync)
        self.assertIn("BufferSyncResult::Ready", range_sync)
        self.assertIn("return synchronized", range_sync)

        bind_body = self.rasterizer.split("bool Rasterizer::BindResources", 1)[1].split(
            "bool Rasterizer::IsComputeMetaClear", 1
        )[0]
        dma_sync = bind_body.split("if (uses_dma)", 1)[1].split(
            "fault_process_pending = true", 1
        )[0]
        self.assertIn("if (!buffer_cache.SynchronizeBuffersInRange", dma_sync)
        self.assertIn("return false", dma_sync)
        self.assertLess(
            dma_sync.index("SynchronizeBuffersInRange"),
            dma_sync.index("return false"),
        )
        self.assertGreaterEqual(self.rasterizer.count("if (!BindResources(pipeline))"), 2)

    def test_image_buffer_obtain_never_bypasses_snapshot_transaction(self):
        image_obtain = self.cache_cpp.split(
            "BufferCache::ObtainBufferForImage", 1
        )[1].split("bool BufferCache::IsRegionRegistered", 1)[0]
        self.assertIn(
            "SynchronizeBuffer(buffer, gpu_addr, size, false, false,",
            image_obtain,
        )
        self.assertIn("false, true, external_snapshot_guard", image_obtain)
        self.assertIn("BufferSyncResult::Failed", image_obtain)
        self.assertIn("if (result.first == &GetBuffer(NULL_BUFFER_ID))", image_obtain)
        self.assertIn("result = {nullptr, 0}", image_obtain)
        self.assertIn("return {nullptr, 0}", image_obtain)
        self.assertGreaterEqual(
            image_obtain.count("return safe_obtain()"), 2
        )
        for raw_copy_token in (
            "staging_buffer.Map",
            "TryCopyBackingMemory",
            "CopySparseMemory",
            "staging_buffer.Commit",
        ):
            self.assertNotIn(raw_copy_token, image_obtain)

    def test_texel_image_alias_failure_is_fail_closed(self):
        sync_body = self.cache_cpp.split(
            "BufferCache::BufferSyncResult BufferCache::SynchronizeBuffer", 1
        )[1].split("vk::Buffer BufferCache::UploadCopies", 1)[0]
        image_alias_gate = sync_body.split(
            "if (is_texel_buffer && !is_written)", 1
        )[1]
        self.assertIn("texture_cache.FindImageFromRange", image_alias_gate)
        compact_image_alias_gate = " ".join(image_alias_gate.split())
        self.assertIn(
            "image_alias && !SynchronizeBufferFromImage",
            compact_image_alias_gate,
        )
        self.assertLess(
            image_alias_gate.index("!SynchronizeBufferFromImage"),
            image_alias_gate.index('return fail("texel-image-copy")'),
        )
        self.assertLess(
            image_alias_gate.index('return fail("texel-image-copy")'),
            image_alias_gate.index("return BufferSyncResult::Ready"),
        )

        obtain_body = self.cache_cpp.split("BufferCache::ObtainBuffer(VAddr", 1)[1].split(
            "BufferCache::ObtainReadOnlyBuffer", 1
        )[0]
        self.assertIn("SynchronizeBuffer(buffer, device_addr, size, is_written", obtain_body)
        failed_path = obtain_body.split("BufferSyncResult::Failed", 1)[1].split(
            "if (is_written)", 1
        )[0]
        self.assertIn("return {&GetBuffer(NULL_BUFFER_ID), 0}", failed_path)

    def test_failed_image_obtain_returns_null_and_preserves_dirty_for_retry(self):
        image_obtain = self.cache_cpp.split(
            "BufferCache::ObtainBufferForImage", 1
        )[1].split("bool BufferCache::IsRegionRegistered", 1)[0]
        normalization = image_obtain.split("const auto safe_obtain", 1)[1].split(
            "const BufferId buffer_id", 1
        )[0]
        self.assertIn(
            "ObtainBuffer(gpu_addr, size, false, false, {}, false,",
            normalization,
        )
        self.assertIn("false, true, external_snapshot_guard", normalization)
        self.assertIn("result.first == &GetBuffer(NULL_BUFFER_ID)", normalization)
        self.assertIn("result = {nullptr, 0}", normalization)

        direct_sync_failure = image_obtain.split(
            "SynchronizeBuffer(buffer, gpu_addr, size, false, false,", 1
        )[1].split("return {&buffer, buffer.Offset(gpu_addr)}", 1)[0]
        self.assertIn("false, true, external_snapshot_guard", direct_sync_failure)
        self.assertIn("BufferSyncResult::Failed", direct_sync_failure)
        self.assertIn("return {nullptr, 0}", direct_sync_failure)

        refresh = self.texture_cache_cpp.split("bool TextureCache::RefreshImage", 1)[1].split(
            "vk::Sampler TextureCache::GetSampler", 1
        )[0]
        acquisition_tail = refresh.split("buffer_cache.ObtainBufferForImage", 1)[1]
        null_guard = acquisition_tail.split("if (in_buffer == nullptr)", 1)[1].split(
            "#ifdef __ANDROID__", 1
        )[0]
        self.assertIn("return false;", null_guard)
        for forbidden_failure_side_effect in (
            "image.flags &=",
            "~ImageFlagBits::Dirty",
            "DetileImage",
            "image.Upload",
        ):
            self.assertNotIn(forbidden_failure_side_effect, null_guard)
        for downstream_operation in (
            "in_buffer->GetBarrier",
            "tile_manager.DetileImage",
            "image.Upload",
        ):
            self.assertLess(
                acquisition_tail.index("return false;"),
                acquisition_tail.index(downstream_operation),
            )


if __name__ == "__main__":
    unittest.main()
