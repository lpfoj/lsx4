import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
VK = ROOT / "funnel-arm" / "src" / "video_core" / "renderer_vulkan"


class VulkanTransferTransactionContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.scheduler_h = (VK / "vk_scheduler.h").read_text(encoding="utf-8")
        cls.scheduler_cpp = (VK / "vk_scheduler.cpp").read_text(encoding="utf-8")
        cls.rasterizer = (VK / "vk_rasterizer.cpp").read_text(encoding="utf-8")
        cls.hle = (VK / "vk_shader_hle.cpp").read_text(encoding="utf-8")
        cls.liverpool = (
            ROOT / "funnel-arm" / "src" / "video_core" / "amdgpu" / "liverpool.cpp"
        ).read_text(encoding="utf-8")
        cls.liverpool_h = (
            ROOT / "funnel-arm" / "src" / "video_core" / "amdgpu" / "liverpool.h"
        ).read_text(encoding="utf-8")
        cls.page_manager = (
            ROOT / "funnel-arm" / "src" / "video_core" / "page_manager.cpp"
        ).read_text(encoding="utf-8")
        cls.native_runtime = (
            ROOT / "src" / "executor" / "native_runtime_api.cpp"
        ).read_text(encoding="utf-8")
        cls.fex_abi = (
            ROOT / "src" / "executor" / "fex_backend_abi_v3.h"
        ).read_text(encoding="utf-8")
        cls.memory_h = (
            ROOT / "funnel-arm" / "src" / "core" / "memory.h"
        ).read_text(encoding="utf-8")
        cls.buffer_cache = (
            ROOT / "funnel-arm" / "src" / "video_core" / "buffer_cache" / "buffer_cache.cpp"
        ).read_text(encoding="utf-8")
        cls.texture_cache = (
            ROOT / "funnel-arm" / "src" / "video_core" / "texture_cache" / "texture_cache.cpp"
        ).read_text(encoding="utf-8")

    def test_uploads_are_deferred_only_by_an_explicit_raii_scope(self):
        self.assertIn("class ScopedBufferUploadBatch", self.scheduler_h)
        self.assertIn("~ScopedBufferUploadBatch()", self.scheduler_h)
        self.assertIn("BeginBufferUploadBatch()", self.scheduler_h)
        self.assertIn("ScopedBufferUploadBatch::~ScopedBufferUploadBatch", self.scheduler_cpp)
        self.assertIn("run-bisect-enable-vk-scoped-upload-batch", self.scheduler_cpp)
        begin_batch = self.scheduler_cpp.split(
            "Scheduler::ScopedBufferUploadBatch Scheduler::BeginBufferUploadBatch()", 1
        )[1].split("void Scheduler::QueueBufferCopies", 1)[0]
        self.assertIn("EXECUTOR_FEX_OWNS_FAULT_SIGNALS", begin_batch)
        self.assertIn("!fex_backend", begin_batch)
        self.assertLess(
            begin_batch.index("EXECUTOR_FEX_OWNS_FAULT_SIGNALS"),
            begin_batch.index("++scoped_buffer_upload_batch_depth"),
        )
        destructor = self.scheduler_cpp.split(
            "ScopedBufferUploadBatch::~ScopedBufferUploadBatch", 1
        )[1].split("void Scheduler::ScopedBufferUploadBatch::Flush", 1)[0]
        self.assertIn("Flush();", destructor)

        queue = self.scheduler_cpp.split("void Scheduler::QueueBufferCopies", 1)[1].split(
            "void Scheduler::QueueBufferUploadCopies", 1
        )[0]
        self.assertIn("origin == BufferTransferOrigin::Upload", queue)
        self.assertIn("scoped_buffer_upload_batch_depth == 0", queue)
        self.assertIn("!VideoCore::IsManagedVulkanTransferCoalescing()", queue)

    def test_overlap_and_capacity_boundaries_still_flush_inside_a_scope(self):
        queue = self.scheduler_cpp.split("void Scheduler::QueueBufferCopies", 1)[1].split(
            "void Scheduler::QueueBufferUploadCopies", 1
        )[0]
        capacity = queue.split("MaxPendingGroups", 1)[1].split(
            "const auto range_end", 1
        )[0]
        self.assertIn("FlushPendingBufferTransfers();", capacity)

        overlap = self.scheduler_cpp.split(
            "void Scheduler::FlushPendingBufferTransfersForRange", 1
        )[1].split("void Scheduler::FlushPendingGnmDmaTransfers", 1)[0]
        self.assertIn("transfer.source_begin", overlap)
        self.assertIn("transfer.destination_begin", overlap)
        self.assertIn("if (has_conflict)", overlap)
        self.assertIn("FlushPendingBufferTransfers();", overlap)

    def test_draw_preparation_separates_resource_and_geometry_publication(self):
        draw = self.rasterizer.split("void Rasterizer::Draw(bool", 1)[1].split(
            "void Rasterizer::DrawIndirect", 1
        )[0]
        self.assertEqual(draw.count("BeginBufferUploadBatch()"), 2)
        self.assertEqual(draw.count("resource_upload_batch.Flush()"), 2)
        self.assertEqual(draw.count("geometry_upload_batch.Flush()"), 1)
        self.assertLess(draw.index("BeginBufferUploadBatch()"), draw.index("BindResources(pipeline)"))
        self.assertLess(draw.index("resource_upload_batch.Flush()"), draw.index("geometry_upload_batch"))
        self.assertLess(draw.index("PrepareVertexBuffers"), draw.index("geometry_upload_batch.Flush()"))
        self.assertLess(draw.index("geometry_upload_batch.Flush()"), draw.index("BeginRendering(pipeline)"))
        self.assertLess(draw.index("geometry_upload_batch.Flush()"), draw.index("pipeline->BindResources"))
        self.assertLess(draw.index("geometry_upload_batch.Flush()"), draw.index("scheduler.BeginRendering(state)"))

    def test_indirect_draw_preparation_publishes_before_indirect_consumption(self):
        draw = self.rasterizer.split("void Rasterizer::DrawIndirect", 1)[1].split(
            "void Rasterizer::DispatchDirect", 1
        )[0]
        self.assertEqual(draw.count("BeginBufferUploadBatch()"), 2)
        self.assertEqual(draw.count("resource_upload_batch.Flush()"), 2)
        self.assertEqual(draw.count("geometry_upload_batch.Flush()"), 1)
        self.assertLess(draw.index("resource_upload_batch.Flush()"), draw.index("geometry_upload_batch"))
        self.assertLess(draw.index("ObtainReadOnlyBuffer"), draw.index("geometry_upload_batch.Flush()"))
        self.assertLess(draw.index("PrepareVertexBuffers"), draw.index("geometry_upload_batch.Flush()"))
        self.assertLess(draw.index("geometry_upload_batch.Flush()"), draw.index("BeginRendering(pipeline)"))
        self.assertLess(draw.index("geometry_upload_batch.Flush()"), draw.index("pipeline->BindResources"))
        self.assertLess(draw.index("geometry_upload_batch.Flush()"), draw.index("scheduler.BeginRendering(state)"))

    def test_compute_preparation_publishes_before_binding_and_dispatch(self):
        direct = self.rasterizer.split("void Rasterizer::DispatchDirect", 1)[1].split(
            "void Rasterizer::DispatchIndirect", 1
        )[0]
        indirect = self.rasterizer.split("void Rasterizer::DispatchIndirect", 1)[1].split(
            "u64 Rasterizer::Flush", 1
        )[0]
        for body in (direct, indirect):
            self.assertEqual(body.count("BeginBufferUploadBatch()"), 1)
            self.assertEqual(body.count("buffer_upload_batch.Flush()"), 2)
            self.assertLess(body.index("BindResources(pipeline)"), body.index("buffer_upload_batch.Flush()"))
            self.assertLess(body.index("buffer_upload_batch.Flush()"), body.index("pipeline->BindResources"))
            self.assertLess(body.index("buffer_upload_batch.Flush()"), body.index("scheduler.CommandBuffer()"))

    def test_managed_hle_batches_source_destination_uploads_with_the_copy(self):
        body = self.hle.split("static bool ExecuteCopyShaderHLE", 1)[1].split(
            "bool ExecuteShaderHLE", 1
        )[0]
        self.assertEqual(body.count("BeginBufferUploadBatch()"), 1)
        self.assertEqual(body.count("buffer_upload_batch.Flush()"), 1)
        self.assertLess(body.index("BeginBufferUploadBatch()"), body.index("ObtainReadOnlyBuffer"))
        self.assertLess(body.index("QueueBufferCopies"), body.index("buffer_upload_batch.Flush()"))

    def test_gpu_thread_token_keeps_exact_tls_fallback(self):
        self.assertIn("run-enable-gpu-thread-token", self.liverpool)
        self.assertIn("__builtin_thread_pointer()", self.liverpool)
        self.assertIn(
            "std::atomic<uintptr_t> gpu_command_thread_token{}", self.liverpool_h
        )
        self.assertIn(
            "static thread_local const Liverpool* gpu_command_thread_owner",
            self.liverpool_h,
        )

        identity = self.liverpool.split(
            "bool Liverpool::IsGpuThread() const noexcept", 1
        )[1].split("void Liverpool::FlushGpuCompletionBatchForExecutor", 1)[0]
        token_check = identity.index("gpu_command_thread_token.load")
        tls_fallback = identity.index("gpu_command_thread_owner == this")
        tid_fallback = identity.index("gpu_command_tid.load")
        self.assertLess(token_check, tls_fallback)
        self.assertLess(tls_fallback, tid_fallback)
        self.assertIn("[EXECUTOR_GPU_THREAD_TOKEN] firstHit=1", identity)

        process = self.liverpool.split(
            "void Liverpool::Process(std::stop_token stoken)", 1
        )[1].split("Liverpool::Task Liverpool::ProcessGraphics", 1)[0]
        self.assertIn("ExecutorGpuThreadTokenEnabled()", process)
        self.assertIn("gpu_command_thread_token.exchange", process)
        self.assertIn("gpu_command_thread_token.store", process)

    def test_completion_labels_use_exact_cpu_sidecar_without_page_protection(self):
        self.assertIn("EXECUTOR_DISABLE_CPU_SIDE_COMPLETION_LABELS", self.liverpool)
        record = self.liverpool.split(
            "void Liverpool::RecordCompletionWriteForExecutor", 1
        )[1].split("void Liverpool::CompletePendingCompletionWriteForExecutor", 1)[0]
        self.assertIn("executor_pending_completion_writes[write]", record)
        self.assertIn("if (!cpu_sidecar_only)", record)
        self.assertIn("executor_pending_completion_watched_writes[write]", record)
        self.assertIn("TrackPendingCompletionRead", record)

        complete = self.liverpool.split(
            "void Liverpool::CompletePendingCompletionWriteForExecutor", 1
        )[1].split("u64 Liverpool::ConsumeCompletionWaitForExecutor", 1)[0]
        self.assertIn("executor_pending_completion_watched_writes.find(key)", complete)
        self.assertIn("if (watched_refs != 0)", complete)
        self.assertIn("NotifyMemoryPublicationForExecutor", complete)

    def test_unresolved_fault_diagnostic_explains_no_progress(self):
        self.assertIn("[EXECUTOR_PAGE_FAULT_UNRESOLVED] reason=%s owner=tracked", self.page_manager)
        self.assertIn('"watcher_denies_access"', self.page_manager)
        self.assertIn('"pte_still_protected"', self.page_manager)
        self.assertIn("page=0x%llx", self.page_manager)

    def test_submit_readback_keeps_dependency_visible_until_page_is_open(self):
        retirement = self.buffer_cache.split(
            "Publish prefetched readbacks on the retirement runner", 1
        )[1].split("void BufferCache::DownloadBufferMemoryWave", 1)[0]
        untrack = retirement.index(
            "page_manager->UntrackPendingCompletionRead(range.begin, range.size)"
        )
        open_watcher = retirement.index(
            "page_manager->UpdatePageWatchers<false>(range.begin, range.size)"
        )
        erase = retirement.index("std::erase_if(pending_readback_ranges")
        self.assertLess(untrack, open_watcher)
        self.assertLess(open_watcher, erase)
        self.assertIn("Keep the retired dependency discoverable", retirement)

    def test_submit_readback_prefetch_has_an_independent_bisect_gate(self):
        self.assertIn(
            "run-bisect-disable-submit-readback-prefetch", self.buffer_cache
        )
        prefetch = self.buffer_cache.split(
            "void BufferCache::PrefetchReadbackWindows()", 1
        )[1].split("void BufferCache::DownloadBufferMemoryWave", 1)[0]
        self.assertIn("!ExecutorSubmitReadbackPrefetchEnabled()", prefetch)
        self.assertLess(
            prefetch.index("!ExecutorSubmitReadbackPrefetchEnabled()"),
            prefetch.index("scheduler.FlushPendingBufferTransfers()"),
        )

    def test_residency_gc_never_recycles_an_unpublished_buffer_generation(self):
        gc = self.buffer_cache.split(
            "void BufferCache::RunGarbageCollector(", 1
        )[1].split("void BufferCache::TouchBuffer", 1)[0]
        self.assertIn("resident_buffer_bytes < trigger_gc_memory", gc)
        self.assertIn("resident_buffer_bytes >= critical_gc_memory", gc)
        self.assertIn("sampled_device_memory", gc)
        self.assertIn("exact_proprietary_adreno750", gc)
        self.assertIn("!instance.CanReportMemoryUsage()", gc)
        self.assertIn("aggregate_pressure", gc)
        self.assertNotIn("GetDeviceMemoryUsage", gc)
        self.assertNotIn("DownloadBufferMemory<true>", self.buffer_cache)
        self.assertIn("static_assert(!async", self.buffer_cache)
        dirty = gc.index("gpu_modified_ranges.Intersects")
        pending = gc.index("HasPendingReadback")
        tracker_dirty = gc.index("memory_tracker->IsRegionGpuModified")
        retain = gc.index("TouchBuffer(buffer)")
        delete = gc.index("DeleteBuffer(buffer_id)")
        self.assertLess(pending, retain)
        self.assertLess(dirty, retain)
        self.assertLess(tracker_dirty, retain)
        self.assertLess(retain, delete)
        self.assertNotIn("WaitForPendingReadback", gc)

        register = self.buffer_cache.split(
            "void BufferCache::ChangeRegister", 1
        )[1].split("bool BufferCache::ResolveSnapshotDependencies", 1)[0]
        self.assertIn("resident_buffer_bytes +=", register)
        self.assertIn("ASSERT(resident_buffer_bytes >= resident_size)", register)
        self.assertIn("resident_buffer_bytes -= resident_size", register)

        submit_gc = self.rasterizer.split(
            "if (run_residency_gc)", 1
        )[1].split("pipeline_cache.AdvanceShaderIdentitySubmit", 1)[0]
        self.assertIn(
            "texture_cache.RunGarbageCollector(cache_pressure_sample)",
            submit_gc,
        )
        self.assertIn(
            "buffer_cache.RunGarbageCollector(cache_pressure_sample)",
            submit_gc,
        )

    def test_texture_gc_uses_global_memory_only_as_near_budget_emergency(self):
        gc = self.texture_cache.split(
            "void TextureCache::GarbageCollectImages", 1
        )[1].split("void TextureCache::GarbageCollectSamplers", 1)[0]
        self.assertIn("maintained incrementally by RegisterImage", gc)
        self.assertIn("const bool device_emergency", gc)
        self.assertNotIn("const bool device_pressure", gc)
        self.assertIn("exact_proprietary_adreno750", gc)
        self.assertIn("? device_budget / 5", gc)
        self.assertIn("device_budget - emergency_headroom", gc)
        self.assertNotIn("for (const Image& image : slot_images)", gc)
        self.assertNotIn("estimated_image_memory", gc)
        self.assertNotIn("std::max<u64>(*sampled_device_memory", gc)
        self.assertNotIn("instance.GetDeviceMemoryUsage()", gc)

    def test_fex_submit_boundary_captures_epochs_before_reopening_writers(self):
        graphics = self.liverpool.split(
            "void Liverpool::SubmitGfx", 1
        )[1].split("if (!PublishSubmit(GfxQueueId", 1)[0]
        compute = self.liverpool.split(
            "void Liverpool::SubmitAsc", 1
        )[1].split("if (!PublishSubmit(gnm_vqid", 1)[0]
        for boundary in (graphics, compute):
            self.assertIn("bool epoch_boundary =", boundary)
            self.assertIn(
                "DrainFexDeferredWriteEpochs(true, 0, 0,\n"
                "                                                            true);",
                boundary,
            )
            self.assertIn("SCOPE_EXIT", boundary)
            self.assertLess(
                boundary.index("EndFexDeferredWriteSubmitBoundary"),
                boundary.index("DrainFexDeferredWriteEpochs"),
            )

    def test_external_guest_stack_uses_a_lock_lease_without_fake_epoch_state(self):
        lease_type = self.native_runtime.split(
            "struct ExecutorFexGuestMemoryLease final", 1
        )[1].split("int32_t ExecutorFexAcquireGuestMemoryLease", 1)[0]
        self.assertIn("std::unique_lock<std::mutex> external_stack_guard", lease_type)
        self.assertIn("external_stack_guard{std::move(guard_)}", lease_type)

        acquire = self.native_runtime.split(
            "int32_t ExecutorFexAcquireGuestMemoryLease", 1
        )[1].split("void ExecutorFexReleaseGuestMemoryLease", 1)[0]
        external = acquire.split(
            "const std::uint32_t external_attempts", 1
        )[1].split("[EXECUTOR_FEX_MEMORY_LEASE_REJECTED]", 1)[0]
        self.assertLess(
            external.index("std::unique_lock stack_lock"),
            external.index("g_box64_external_guest_stack_ranges"),
        )
        self.assertLess(
            external.index("g_box64_external_guest_stack_ranges"),
            external.index("std::move(stack_lock)"),
        )
        strict = external.split("if (strict_non_reentrant) {", 1)[1].split(
            "} else", 1
        )[0]
        self.assertIn("lease.release()", strict)
        self.assertIn("LSX4_FEX_STATUS_OK", strict)
        for forbidden in (
            "TryBeginFexDeferredBulkGuestWrite",
            "TryPrepareFexDeferredBulkGuestWrite",
            "PrepareBulkGuestWrite",
            "EnterBulkGuestWrite",
            "BeginBulkGuestWrite",
            "bulk_write_active = true",
        ):
            self.assertNotIn(forbidden, strict)
        ordinary = external.split("} else {", 1)[1]
        for required in (
            "PrepareBulkGuestWrite",
            "EnterBulkGuestWrite",
            "BeginBulkGuestWrite",
            "lease->bulk_write_active = true",
        ):
            self.assertIn(required, ordinary)

    def test_fex_buffer_capture_ack_linearizes_snapshot_before_reopening_writers(self):
        sync = self.buffer_cache.split(
            "BufferCache::BufferSyncResult BufferCache::SynchronizeBuffer", 1
        )[1].split("vk::Buffer BufferCache::UploadCopies", 1)[0]
        self.assertIn("run-enable-fex-buffer-capture-ack", self.native_runtime)
        self.assertIn("FexWriteEpochState::Capturing", self.page_manager)
        self.assertIn("FexWriteEpochPressureThreshold", self.page_manager)

        prepare_bulk = self.page_manager.split(
            "void PrepareBulkGuestWrite(const VAddr address, const u64 size) {", 1
        )[1].split("#if 0", 1)[0]
        self.assertIn("ordered_global_admission", prepare_bulk)
        self.assertLess(
            prepare_bulk.index("invalidate_protected_pages();"),
            prepare_bulk.index("AdmitCurrentUntrackedFexBulkWriter(address, size)"),
        )
        self.assertLess(
            prepare_bulk.index("AdmitCurrentUntrackedFexBulkWriter(address, size)"),
            prepare_bulk.index("EnterTrackedBulkWriteGate(address, size)"),
        )
        self.assertLess(
            prepare_bulk.index("LeaveTrackedBulkWriteGate();"),
            prepare_bulk.index("ReleaseCurrentFexBulkWriterAdmission();"),
        )

        snapshot_boundary = self.page_manager.split(
            "bool PageManager::BeginFexBufferSnapshotBoundary", 1
        )[1].split("void PageManager::EndFexBufferSnapshotBoundary", 1)[0]
        close_single = snapshot_boundary.index(
            "epochs.gate_open.store(false, std::memory_order_release);"
        )
        wait_faulted = snapshot_boundary.index("has_overlapping_faulted_epoch()")
        kick_faulted = snapshot_boundary.index(
            "kick_overlapping_faulted_owners();"
        )
        close_bulk = snapshot_boundary.index(
            "epochs.bulk_gate_open.store(false, std::memory_order_release);"
        )
        drain = snapshot_boundary.index("DrainFexDeferredWriteEpochs(")
        self.assertLess(close_single, wait_faulted)
        self.assertLess(close_single, kick_faulted)
        self.assertLess(kick_faulted, wait_faulted)
        self.assertLess(wait_faulted, close_bulk)
        self.assertLess(close_bulk, drain)
        self.assertIn("safe_point_deadline", snapshot_boundary)
        self.assertIn(
            "executor_lsx4_android_fex_deferred_write_epoch_kick_owner",
            snapshot_boundary,
        )

        reserve = self.page_manager.split(
            "FexWriteEpochSlot* ReserveFexWriteEpoch", 1
        )[1].split("FexWriteEpochSlot* AcquireFexBulkPageEpoch", 1)[0]
        self.assertIn(
            "epochs.pending.load(std::memory_order_acquire) >=\n"
            "            FexWriteEpochPressureThreshold",
            reserve,
        )
        self.assertIn("epochs.fallbacks.fetch_add", reserve)
        self.assertIn("return nullptr;", reserve)

        submit_boundary = self.page_manager.split(
            "bool PageManager::BeginFexDeferredWriteSubmitBoundary()", 1
        )[1].split("void PageManager::EndFexDeferredWriteSubmitBoundary", 1)[0]
        self.assertIn("const bool capture_ack = ExecutorFexBufferCaptureAckEnabled();", submit_boundary)
        self.assertIn("FexWriteEpochPressureThreshold", submit_boundary)
        self.assertLess(
            submit_boundary.index("if (capture_ack &&"),
            submit_boundary.index("epochs.drain_mutex.lock()"),
        )
        self.assertGreaterEqual(submit_boundary.count("if (capture_ack &&"), 2)
        self.assertIn("epochs.drain_mutex.unlock();\n        return false;", submit_boundary)

        self.assertIn("CurrentFexDeferredBufferCaptureDirtyPages", sync)
        self.assertIn("const bool fex_capture_hazard = !fex_texture_cache_lock_held &&", sync)
        self.assertIn("if (fex_capture_hazard)", sync)
        self.assertIn('return fail("fex-capture-boundary-unavailable");', sync)
        self.assertLess(
            sync.index("if (external_snapshot_guard == nullptr)"),
            sync.index("fex_snapshot_guard.emplace(memory->AcquireSnapshotReadGuard())"),
        )
        self.assertLess(
            sync.index("fex_snapshot_guard.emplace(memory->AcquireSnapshotReadGuard())"),
            sync.index("PageManager::BeginFexDeferredBufferCapture"),
        )
        self.assertIn("auto* memory_guard = external_snapshot_guard", sync)
        self.assertIn("auto* active_guard = external_snapshot_guard", sync)
        self.assertIn(
            "ResolveSnapshotDependencies(device_addr, size,\n"
            "                                     external_snapshot_guard)",
            sync,
        )
        self.assertIn(
            "const bool exact_dirty_upload_reservation =\n"
            "        upload_policy.upload_reservation ==\n"
            "            Vulkan::UploadReservationMode::ExactDirtyRanges &&\n"
            "        (upload_policy.upload_reservation_scope ==\n"
            "             Vulkan::UploadReservationScope::AllCpuDirtyUploads ||\n"
            "         fex_buffer_capture || fex_snapshot_boundary);",
            sync,
        )
        self.assertIn(
            "upload_needed && !exact_dirty_upload_reservation",
            sync,
        )
        sparse_transaction = sync.split(
            "for (u32 attempt = 0; attempt < MaxSnapshotTransactionAttempts",
            1,
        )[1].split("const auto action = DecideSnapshotTransactionAction", 1)[0]
        self.assertEqual(
            sparse_transaction.count("memory_tracker->MeasureUploadRanges("), 1
        )
        self.assertEqual(
            sparse_transaction.count("memory_tracker->ForEachUploadRange("), 1
        )
        self.assertLess(
            sparse_transaction.index("memory_tracker->MeasureUploadRanges("),
            sparse_transaction.index("memory_tracker->ForEachUploadRange("),
        )
        self.assertLess(
            sparse_transaction.index(
                "reserve_upload_storage(\n                                    measured_upload_bytes"
            ),
            sparse_transaction.index("memory_tracker->ForEachUploadRange("),
        )
        sparse_retry = sparse_transaction.split(
            "exact_dirty_upload_reservation && sparse_upload_plan_changed", 1
        )[1]
        self.assertIn("continue;", sparse_retry)
        self.assertNotIn("release_fex_buffer_capture", sparse_retry)
        self.assertLess(
            sync.index("total_size_bytes += range_size;"),
            sync.index("if (upload_data == nullptr ||"),
        )
        self.assertIn('return fail("fex-capture-retry", transaction_result);', sync)
        release = sync.split(
            "const auto release_fex_buffer_capture", 1
        )[1].split("SCOPE_EXIT", 1)[0]
        self.assertLess(
            release.index("PageManager::EndFexDeferredBufferCapture("),
            release.index("fex_snapshot_guard.reset()"),
        )
        self.assertIn("true, fex_texture_cache_lock_held", release)
        image_obtain = self.buffer_cache.split(
            "BufferCache::ObtainBufferForImage", 1
        )[1].split("bool BufferCache::IsRegionRegistered", 1)[0]
        self.assertIn(
            "false, false, {}, false,\n"
            "                                   false, true, external_snapshot_guard",
            image_obtain,
        )
        self.assertIn(
            "false, false,\n"
            "                                  false, true, external_snapshot_guard",
            image_obtain,
        )

        ordered_refresh = self.texture_cache.split(
            "bool TextureCache::RefreshImageOrdered", 1
        )[1].split("bool TextureCache::RefreshImage(", 1)[0]
        self.assertLess(
            ordered_refresh.index("lock.unlock()"),
            ordered_refresh.index(
                "PageManager::PrepareTextureGuestReadProtectedRuns"
            ),
        )
        self.assertLess(
            ordered_refresh.index(
                "PageManager::PrepareTextureGuestReadProtectedRuns"
            ),
            ordered_refresh.index("guest_memory->AcquireSnapshotReadGuard()"),
        )
        self.assertLess(
            ordered_refresh.index("guest_memory->AcquireSnapshotReadGuard()"),
            ordered_refresh.index("PageManager::BeginFexBufferSnapshotBoundary"),
        )
        self.assertLess(
            ordered_refresh.index("PageManager::BeginFexBufferSnapshotBoundary"),
            ordered_refresh.rindex("lock.lock()"),
        )
        self.assertLess(
            ordered_refresh.rindex("lock.lock()"),
            ordered_refresh.index("RefreshImage(image, snapshot_guard ? &*snapshot_guard"),
        )
        self.assertIn("PageManager::EndFexBufferSnapshotBoundary", ordered_refresh)
        self.assertIn(
            "return RefreshImage(image, snapshot_guard ? &*snapshot_guard : nullptr);",
            ordered_refresh,
        )

        dependency_resolve = self.buffer_cache.split(
            "bool BufferCache::ResolveSnapshotDependencies", 1
        )[1].split("bool BufferCache::ResolveSnapshotGpuRange", 1)[0]
        self.assertIn("auto* memory_guard = external_snapshot_guard", dependency_resolve)
        self.assertIn("if (memory_guard == nullptr)", dependency_resolve)
        self.assertIn("*memory_guard, device_addr, size", dependency_resolve)

        capture_begin = self.page_manager.split(
            "bool PageManager::BeginFexDeferredBufferCapture", 1
        )[1].split("void PageManager::EndFexDeferredBufferCapture", 1)[0]
        capture_success = capture_begin.split("ticket.active = true", 1)[1]
        self.assertIn("coordinator_lock.release()", capture_success)
        self.assertNotIn("reopen_gates();", capture_success)
        capture_end = self.page_manager.split(
            "void PageManager::EndFexDeferredBufferCapture", 1
        )[1].split("bool PageManager::BeginFexBufferSnapshotBoundary", 1)[0]
        self.assertLess(
            capture_end.index("epochs.gate_open.store(true"),
            capture_end.index("epochs.drain_mutex.unlock()"),
        )

        snapshot_boundary = self.page_manager.split(
            "bool PageManager::BeginFexBufferSnapshotBoundary", 1
        )[1].split("void PageManager::EndFexBufferSnapshotBoundary", 1)[0]
        self.assertIn("epochs.gate_open.store(false", snapshot_boundary)
        self.assertIn("epochs.bulk_gate_open.store(false", snapshot_boundary)
        self.assertIn(
            "DrainFexDeferredWriteEpochs(true, address, size, true);",
            snapshot_boundary,
        )

        capture_end = self.page_manager.split(
            "void PageManager::EndFexDeferredBufferCapture", 1
        )[1].split("bool PageManager::BeginFexBufferSnapshotBoundary", 1)[0]
        self.assertLess(
            capture_end.index("PublishCapturedGuestBufferWritePages"),
            capture_end.index("slot.state.store(static_cast<u32>(FexWriteEpochState::Free)"),
        )
        self.assertLess(
            capture_end.index("slot.state.store(static_cast<u32>(FexWriteEpochState::Free)"),
            capture_end.index("epochs.gate_open.store(true"),
        )
        publish = self.rasterizer.split(
            "void Rasterizer::PublishCapturedGuestBufferWritePages", 1
        )[1].split("bool Rasterizer::IsGpuThread", 1)[0]
        self.assertIn("pipeline_cache.InvalidateShaderIdentityRange", publish)
        self.assertIn("texture_cache.InvalidateMemory", publish)
        self.assertIn("texture_cache.InvalidateMemoryLocked", publish)
        self.assertIn("NotifyMemoryPublicationForExecutor", publish)

        bulk_acquire = self.page_manager.split(
            "FexWriteEpochSlot* AcquireFexBulkPageEpoch", 1
        )[1].split("void ReleaseFexBulkPageEpoch", 1)[0]
        self.assertIn("AddFexPendingPageRef(slot.address, token)", bulk_acquire)
        self.assertIn("epochs.active_bulk_armers.load", self.page_manager)

        expand = self.texture_cache.split(
            "ImageId TextureCache::ExpandImage", 1
        )[1].split("ImageId TextureCache::FindImage", 1)[0]
        self.assertIn("const u64 source_uid", expand)
        self.assertIn("slot_images[image_id].image_uid != source_uid", expand)
        self.assertIn("slot_images[image_id].info.guest_address != source_address", expand)
        cleanup = expand.split("if (!RefreshImageOrdered", 1)[1].split("return {};", 1)[0]
        self.assertIn("slot_images[new_image_id].image_uid == new_image_uid", cleanup)

        find_image = self.texture_cache.split(
            "ImageId TextureCache::FindImage", 1
        )[1].split("ImageId TextureCache::FindImageFromRange", 1)[0]
        self.assertIn("struct ImageCandidate", find_image)
        self.assertIn("candidate_image.image_uid", find_image)
        self.assertIn("slot_images[cache_id].image_uid != candidate.uid", find_image)
        self.assertIn("const ImageInfo merged_info", find_image)
        self.assertIn("slot_images[image_id].image_uid != image_uid", find_image)

        for name in ("FindTexture", "FindRenderTarget", "FindDepthTarget"):
            section = self.texture_cache.split(
                f"ImageView* TextureCache::{name}", 1
            )[1].split("\n}\n", 1)[0]
            self.assertIn("std::unique_lock lock{mutex}", section)
            self.assertIn("const u64 image_uid", section)
            self.assertIn("slot_images[image_id].image_uid != image_uid", section)

        self.assertIn("++published_command_epoch", self.liverpool_h)
        self.assertIn("queue.submits.emplace(handle, published_command_epoch)", self.liverpool)
        self.assertIn("completed_command_epoch = command_epoch", self.liverpool)
        self.assertIn(
            "completed_command_epoch < active_task_command_epochs[curr_qid]",
            self.liverpool,
        )

    def test_texture_hash_preflight_reads_only_protected_page_runs(self):
        ordered_refresh = self.texture_cache.split(
            "bool TextureCache::RefreshImageOrdered", 1
        )[1].split("bool TextureCache::RefreshImage(", 1)[0]
        self.assertIn(
            "PageManager::PrepareTextureGuestReadProtectedRuns",
            ordered_refresh,
        )
        self.assertNotIn("PageManager::PrepareBulkGuestRead(", ordered_refresh)

        sparse = self.page_manager.split(
            "bool PrepareTextureGuestReadProtectedRuns", 1
        )[1].split("bool IsGpuThread() const noexcept", 1)[0]
        self.assertIn(
            "while (page < page_end && !ActualReadProtected(page))", sparse
        )
        self.assertIn(
            "while (page < page_end && ActualReadProtected(page))", sparse
        )
        self.assertIn("Common::RangeLockGuard lk(lock_start, lock_end)", sparse)
        self.assertIn("const VAddr run_begin", sparse)
        self.assertIn("const VAddr run_end", sparse)
        self.assertIn("ReadMemoryFromGuestThread(run.begin", sparse)
        self.assertNotIn(
            "ReadMemoryFromGuestThread(address, bounded_size)", sparse
        )
        self.assertIn("constexpr u32 MaxReadbackPasses = 2", sparse)
        self.assertIn("read_ready = collect_protected_runs()", sparse)
        self.assertIn("return read_ready;", sparse)
        self.assertIn("[EXECUTOR_TEXTURE_READ_PREFLIGHT]", sparse)
        self.assertIn("totalAvoidedBytes=%llu", sparse)

        bulk = self.page_manager.split("void PrepareBulkGuestRead", 1)[1].split(
            "bool PrepareTextureGuestReadProtectedRuns", 1
        )[0]
        self.assertIn(
            "ReadMemoryFromGuestThread(address, bounded_size)", bulk
        )

        self.assertIn(
            "!PageManager::PrepareTextureGuestReadProtectedRuns",
            ordered_refresh,
        )
        self.assertIn(
            "!PageManager::IsFexGuestMemoryReadReady", ordered_refresh
        )
        self.assertIn("const GuestBackingHashPlan initial_hash_plan", ordered_refresh)
        self.assertIn("const u64 hash_read_size = initial_hash_plan.read_size", ordered_refresh)
        self.assertIn("if (hash_read_size != 0 &&", ordered_refresh)
        self.assertGreaterEqual(ordered_refresh.count("hash_read_size"), 6)
        self.assertNotIn(
            "PrepareTextureGuestReadProtectedRuns(guest_address,\n"
            "                                                           guest_size)",
            ordered_refresh,
        )
        self.assertIn(
            "BuildGuestBackingHashPlan(image).read_size > hash_read_size",
            ordered_refresh,
        )
        failed_preflight = ordered_refresh.split(
            "!PageManager::PrepareTextureGuestReadProtectedRuns", 1
        )[1].split("#endif", 1)[0]
        self.assertIn("lock.lock();", failed_preflight)
        self.assertIn("return false;", failed_preflight)
        post_guard = ordered_refresh.split(
            "!PageManager::IsFexGuestMemoryReadReady", 1
        )[1].split("#endif", 1)[0]
        self.assertLess(
            post_guard.index("snapshot_guard.reset()"),
            post_guard.index("lock.lock()"),
        )
        self.assertIn("return false;", post_guard)

        hash_plan = self.texture_cache.split(
            "GuestBackingHashPlan BuildGuestBackingHashPlan", 1
        )[1].split("static void TraceGuestBackingHashPlan", 1)[0]
        self.assertIn("const bool gpu_dirty", hash_plan)
        self.assertGreaterEqual(hash_plan.count("!gpu_dirty"), 2)
        self.assertIn("return {};", hash_plan)
        self.assertIn("GuestBackingHashMode::Sample", hash_plan)
        self.assertIn("GuestBackingHashMode::FullMip", hash_plan)
        self.assertIn("[EXECUTOR_TEXTURE_HASH_PLAN]", self.texture_cache)

        refresh = self.texture_cache.split(
            "bool TextureCache::RefreshImage(", 1
        )[1].split("vk::Sampler TextureCache::GetSampler", 1)[0]
        self.assertIn("const GuestBackingHashPlan hash_plan", refresh)
        self.assertIn("if (hash_plan.sample_hash)", refresh)
        self.assertIn("if (hash_plan.mip_hash)", refresh)
        self.assertNotIn(
            "if (is_gpu_modified && !is_gpu_dirty)", refresh
        )

    def test_fex_capture_safety_conservatively_rejects_unstable_image_snapshots(self):
        may_have = self.page_manager.split(
            "bool PageManager::MayHaveFexDeferredBufferCapture", 1
        )[1].split(
            "PageManager::CurrentFexDeferredBufferCaptureDirtyPages", 1
        )[0]
        for active_writer in (
            "epochs.active_bulk_armers.load(std::memory_order_acquire)",
            "epochs.active_bulk_writers.load(std::memory_order_acquire)",
            "epochs.active_untracked_bulk_writers.load(std::memory_order_acquire)",
            "owner->tracked_bulk_writers.load(std::memory_order_acquire)",
            "owner->active_unwatched_bulk_writers.load(std::memory_order_acquire)",
        ):
            self.assertIn(active_writer, may_have)
        self.assertLess(
            may_have.index("epochs.active_bulk_writers.load"),
            may_have.index("for (VAddr page = page_begin"),
        )
        unwatched_admission = self.page_manager.split(
            "bool TryEnterUnwatchedBulkGuestWrite", 1
        )[1].split("bool LeaveUnwatchedBulkGuestWrite", 1)[0]
        self.assertIn("steady_state_unwatched_disabled", unwatched_admission)
        self.assertIn(
            "initial_gpu_mapping_registration_complete.load(",
            unwatched_admission,
        )
        self.assertIn(
            "if (steady_state_unwatched_disabled ||",
            unwatched_admission,
        )
        self.assertIn("completed_during_admission", unwatched_admission)
        enter_bulk = self.page_manager.split(
            "void PageManager::EnterBulkGuestWrite", 1
        )[1].split("void PageManager::LeaveBulkGuestWrite", 1)[0]
        self.assertIn("bootstrap_registration", enter_bulk)
        self.assertIn(
            "InitialGpuMappingRegistrationComplete", enter_bulk
        )
        oversized_may_have = may_have.split(
            "ExecutorFexBufferCaptureTicket::PageCapacity", 1
        )[1].split("for (VAddr page = page_begin", 1)[0]
        self.assertIn(
            "return epochs.pending.load(std::memory_order_acquire) != 0;",
            oversized_may_have,
        )

        capture_begin = self.page_manager.split(
            "bool PageManager::BeginFexDeferredBufferCapture", 1
        )[1].split("void PageManager::EndFexDeferredBufferCapture", 1)[0]
        oversized_begin = capture_begin.split(
            "ExecutorFexBufferCaptureTicket::PageCapacity", 1
        )[1].split("for (;;) {", 1)[0]
        self.assertIn("return false;", oversized_begin)

        ordered_refresh = self.texture_cache.split(
            "bool TextureCache::RefreshImageOrdered", 1
        )[1].split("bool TextureCache::RefreshImage(", 1)[0]
        after_relock = ordered_refresh.split("lock.lock();", 1)[1]
        self.assertIn(
            "False(image.flags & ImageFlagBits::Registered)",
            after_relock,
        )
        self.assertLess(
            after_relock.index("False(image.flags & ImageFlagBits::Registered)"),
            after_relock.index(
                "return RefreshImage(image, snapshot_guard ? &*snapshot_guard"
            ),
        )

        expand = self.texture_cache.split(
            "ImageId TextureCache::ExpandImage", 1
        )[1].split("ImageId TextureCache::FindImage", 1)[0]
        cleanup = expand.split("if (!RefreshImageOrdered", 1)[1].split(
            "return {};", 1
        )[0]
        self.assertIn(
            "True(slot_images[new_image_id].flags & ImageFlagBits::Registered)",
            cleanup,
        )

        find_from_range = self.texture_cache.split(
            "ImageId TextureCache::FindImageFromRange", 1
        )[1].split("bool TextureCache::RefreshImageById", 1)[0]
        self.assertIn("std::scoped_lock lock{mutex};", find_from_range)
        self.assertIn(
            "False(image.flags & ImageFlagBits::Registered)",
            find_from_range,
        )
        self.assertLess(
            find_from_range.index("False(image.flags & ImageFlagBits::Registered)"),
            find_from_range.index("image_ids.push_back(image_id)"),
        )

    def test_fex_deferred_bulk_write_requires_a_non_reentrant_lease(self):
        self.assertIn(
            "LSX4_FEX_GUEST_MEMORY_ACCESS_NO_HOST_REENTRY",
            self.fex_abi,
        )
        acquire = self.native_runtime.split(
            "int32_t ExecutorFexAcquireGuestMemoryLease", 1
        )[1].split("void ExecutorFexReleaseGuestMemoryLease", 1)[0]
        self.assertIn("constexpr std::uint32_t permission_flags", acquire)
        self.assertIn("constexpr std::uint32_t valid_flags", acquire)
        self.assertIn("(access_flags & permission_flags) == 0", acquire)
        self.assertIn(
            "LSX4_FEX_GUEST_MEMORY_ACCESS_NO_HOST_REENTRY",
            acquire,
        )
        self.assertIn("const bool no_host_reentry", acquire)
        self.assertIn("const bool strict_non_reentrant", acquire)
        self.assertIn("FexDeferredWriteEpochEnabled()", acquire)
        self.assertIn("MaxDeferredLeaseAttempts = 64", acquire)
        self.assertEqual(
            acquire.count(
                "TryBeginFexDeferredBulkGuestWrite"
            ),
            1,
        )
        self.assertEqual(
            acquire.count("IsFexGuestMemoryReadReady"),
            1,
        )
        self.assertEqual(
            acquire.count("PageManager::PrepareBulkGuestWrite"),
            2,
        )
        self.assertEqual(
            self.native_runtime.count(
                "TryBeginFexDeferredBulkGuestWrite"
            ),
            1,
        )
        self.assertNotIn(
            "PageManager::TryPrepareFexDeferredBulkGuestWrite",
            acquire,
        )
        self.assertNotIn("deferred_write", acquire)
        self.assertEqual(
            acquire.count("PageManager::EnterBulkGuestWrite"),
            2,
        )

        tagged_branches = acquire.split("if (strict_non_reentrant) {")[1:]
        self.assertEqual(len(tagged_branches), 2)
        managed = tagged_branches[0].split("} else", 1)[0]
        external_stack = tagged_branches[1].split("} else", 1)[0]
        self.assertIn("SnapshotGpuTrackingActive", managed)
        self.assertIn("TryBeginFexDeferredBulkGuestWrite", managed)
        self.assertIn("IsFexGuestMemoryReadReady", managed)
        self.assertIn("LSX4_FEX_STATUS_BACKEND_FAILURE", managed)
        self.assertNotIn("TryBeginFexDeferredBulkGuestWrite", external_stack)
        self.assertNotIn("IsFexGuestMemoryReadReady", external_stack)
        self.assertIn("lease.release()", external_stack)
        self.assertIn("LSX4_FEX_STATUS_OK", external_stack)
        for tagged in (managed, external_stack):
            self.assertNotIn("PrepareBulkGuestRead", tagged)
            self.assertNotIn("PrepareBulkGuestWrite", tagged)
            self.assertNotIn("EnterBulkGuestWrite", tagged)

        transaction = self.page_manager.split(
            "bool PageManager::TryBeginFexDeferredBulkGuestWrite", 1
        )[1].split("void PageManager::EnterBulkGuestWrite", 1)[0]
        self.assertIn("TryPrepareFexDeferredBulkGuestWrite", transaction)
        self.assertIn("TryEnterFexDeferredBulkGuestWriteActivity", transaction)
        self.assertIn("AbortCurrentFexBulkLeaseAfter(captured_epoch)", transaction)
        self.assertIn("require_read_ready", transaction)
        self.assertIn(
            "require_read_ready && ActualReadProtected",
            self.page_manager,
        )
        self.assertIn(
            "executor_fex_bulk_lease_phase = ExecutorFexBulkLeasePhase::Entered",
            transaction,
        )

        wrapper = self.page_manager.split(
            "bool PageManager::TryPrepareFexDeferredBulkGuestWrite", 1
        )[1].split("void PageManager::EnterBulkGuestWrite", 1)[0]
        self.assertNotIn("ExecutorFexBufferCaptureAckEnabled", wrapper)
        self.assertIn("owner->TryPrepareFexDeferredBulkGuestWrite", wrapper)

        submit_boundary = self.page_manager.split(
            "bool PageManager::BeginFexDeferredWriteSubmitBoundary", 1
        )[1].split("void PageManager::EndFexDeferredWriteSubmitBoundary", 1)[0]
        self.assertIn(
            "executor_fex_bulk_lease_phase != ExecutorFexBulkLeasePhase::Idle",
            submit_boundary,
        )
        self.assertIn(
            "std::unique_lock<std::mutex> topology_lock",
            self.memory_h,
        )
        self.assertIn("std::mutex unmap_mutex", self.memory_h)


if __name__ == "__main__":
    unittest.main()
