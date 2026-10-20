from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
NATIVE = (ROOT / "src/executor/native_runtime_api.cpp").read_text(encoding="utf-8")
STUBS = (ROOT / "funnel-arm/src/core/aerolib/stubs.cpp").read_text(encoding="utf-8")
PAGE = (ROOT / "funnel-arm/src/video_core/page_manager.cpp").read_text(encoding="utf-8")
PIPELINE = (
    ROOT / "funnel-arm/src/video_core/renderer_vulkan/vk_pipeline_cache.cpp"
).read_text(encoding="utf-8")
PIPELINE_HEADER = (
    ROOT / "funnel-arm/src/video_core/renderer_vulkan/vk_pipeline_cache.h"
).read_text(encoding="utf-8")
TEXTURE = (
    ROOT / "funnel-arm/src/video_core/texture_cache/texture_cache.cpp"
).read_text(encoding="utf-8")


def scope(source: str, start: str, end: str) -> str:
    return source.split(start, 1)[1].split(end, 1)[0]


PREPARE = scope(
    PAGE,
    "    bool TryPrepareHleCleanBufferWriteForMemset(",
    "\n    void PrepareBulkGuestWrite(const VAddr address, const u64 size)",
)
RETAIN = scope(
    PIPELINE,
    "bool PipelineCache::RetainHleShaderReadResidency(",
    "\nstd::optional<Shader::ShaderParams> PipelineCache::GetShaderParams",
)
GET_PARAMS = scope(
    PIPELINE,
    "std::optional<Shader::ShaderParams> PipelineCache::GetShaderParams(",
    "\nvoid PipelineCache::InvalidateShaderIdentityRange",
)
GET_PROGRAM = scope(
    PIPELINE,
    "std::optional<PipelineCache::Result> PipelineCache::GetProgram(",
    "\nstd::optional<vk::ShaderModule> PipelineCache::ReplaceShader",
)
ASYNC_SHADER_WORKER = scope(
    PIPELINE,
    "void PipelineCache::AsyncShaderWorker(",
    "\nvoid PipelineCache::PublishAsyncShaderResults",
)
QUEUE_ASYNC_SHADER = scope(
    PIPELINE,
    "bool PipelineCache::QueueAsyncShaderOptimization(",
    "\nvoid PipelineCache::AsyncShaderWorker",
)
COMPILE_MODULE = scope(
    PIPELINE,
    "vk::ShaderModule PipelineCache::CompileModule(",
    "\nstd::optional<PipelineCache::Result> PipelineCache::GetProgram",
)
BEGIN_SHADER_WRITE = scope(
    PIPELINE,
    "bool PipelineCache::BeginHleCleanBufferWrite(",
    "\nvoid PipelineCache::EndHleCleanBufferWrite",
)
REGISTER_IMAGE = scope(
    TEXTURE,
    "void TextureCache::RegisterImage(ImageId image_id)",
    "\nvoid TextureCache::UnregisterImage",
)
DOWNLOAD_ASYNC = scope(
    TEXTURE,
    "        std::shared_ptr<HleResidencyLease> residency_lease;",
    "\n    }\n    return true;",
)


class HleCleanBufferFastPathContractTests(unittest.TestCase):
    def test_default_off_marker_is_explicit_and_memset_only(self):
        self.assertIn("run-disable-async-shader-retranslation", NATIVE)
        self.assertIn(
            'setenv("EXECUTOR_DISABLE_ASYNC_SHADER_RETRANSLATION", "1", 1)',
            NATIVE,
        )
        self.assertIn(
            'unsetenv("EXECUTOR_DISABLE_ASYNC_SHADER_RETRANSLATION")', NATIVE
        )
        self.assertIn(
            "[EXECUTOR_ASYNC_SHADER_RETRANSLATION] source=sentinel", NATIVE
        )
        self.assertIn("enabled=0 disabled=1", NATIVE)
        self.assertIn("run-enable-hle-clean-buffer-fastpath", NATIVE)
        self.assertIn('setenv("EXECUTOR_HLE_CLEAN_BUFFER_FASTPATH", "1", 1)', NATIVE)
        self.assertIn('unsetenv("EXECUTOR_HLE_CLEAN_BUFFER_FASTPATH")', NATIVE)
        self.assertIn("scope=memset-only maxPages=16 default=off", NATIVE)
        self.assertEqual(STUBS.count("PrepareBulkGuestWriteForMemset(dest, size)"), 1)
        for name in ("ExecutorLibcMemcpy", "ExecutorLibcMemmove"):
            body = scope(STUBS, f"u64 {name}", "\n}")
            self.assertNotIn("PrepareBulkGuestWriteForMemset", body)

    def test_hle_and_async_retranslation_share_only_owned_snapshots(self):
        page_gate = scope(
            PAGE,
            "bool ExecutorHleCleanBufferFastPathEnabled()",
            "\nbool ExecutorGuestInvalidationSourceProfileEnabled",
        )
        self.assertIn('std::getenv("EXECUTOR_HLE_CLEAN_BUFFER_FASTPATH")', page_gate)
        self.assertIn("return requested;", page_gate)
        self.assertNotIn("EXECUTOR_DISABLE_ASYNC_SHADER_RETRANSLATION", page_gate)
        self.assertIn("bool async_shader_retranslation_disabled{}", PIPELINE_HEADER)
        constructor = scope(
            PIPELINE,
            "PipelineCache::PipelineCache(",
            "\nPipelineCache::~PipelineCache",
        )
        self.assertIn(
            'std::getenv("EXECUTOR_DISABLE_ASYNC_SHADER_RETRANSLATION")',
            constructor,
        )
        self.assertIn(
            "hle_clean_buffer_guard_enabled = hle_clean_buffer_guard_requested;",
            constructor,
        )
        self.assertIn(
            "[EXECUTOR_ASYNC_SHADER_RETRANSLATION_CONFIG] disabled=%u",
            constructor,
        )
        self.assertIn("fastTierAllowed=%u", constructor)
        self.assertIn("hleGuardEnabled=%u", constructor)
        native_request = NATIVE.index(
            "const bool hle_clean_buffer_fastpath_requested"
        )
        native_admission = NATIVE.index(
            "if (hle_clean_buffer_fastpath_requested)",
            native_request,
        )
        native_enable = NATIVE.index(
            'setenv("EXECUTOR_HLE_CLEAN_BUFFER_FASTPATH", "1", 1)',
            native_admission,
        )
        self.assertLess(native_admission, native_enable)
        self.assertIn("ownedSnapshot=1", NATIVE[native_admission:native_enable + 800])




        for token in (
            "std::ranges::copy(info.user_data, shader_payload->user_data.begin())",
            "shader_payload->code.assign(code.begin(), code.end())",
            "shader_payload->source_runtime_info = runtime_info",
            "const Shader::ShaderParams owned_params",
        ):
            self.assertIn(token, COMPILE_MODULE)
        for token in (
            "payload->user_data",
            "payload->code",
            "payload->source_runtime_info",
        ):
            self.assertIn(token, ASYNC_SHADER_WORKER)
        for forbidden in (
            "Core::Memory",
            "GetShaderParams(",
            "BuildRuntimeInfo(",
            "ResolveFetchShader(",
            "ResolveTessConstants(",
        ):
            self.assertNotIn(forbidden, ASYNC_SHADER_WORKER)

    def test_async_shader_retranslation_is_unreachable_when_disabled(self):
        use_fast_tier = scope(
            COMPILE_MODULE,
            "bool use_fast_tier =",
            "\n    if (use_fast_tier &&",
        )
        self.assertIn("!async_shader_retranslation_disabled", use_fast_tier)
        self.assertLess(
            use_fast_tier.index("!async_shader_retranslation_disabled"),
            use_fast_tier.index("optimization_payload != nullptr"),
        )
        rejection = scope(
            QUEUE_ASYNC_SHADER,
            "if (async_shader_retranslation_disabled)",
            "\n    if (!payload",
        )
        self.assertIn("DEBUG_ASSERT_MSG(false", rejection)
        self.assertIn("return false;", rejection)
        self.assertNotIn(
            "HleCleanBufferGuestReadScope hle_guest_read_scope{*this}",
            ASYNC_SHADER_WORKER,
        )
        worker_start = scope(
            PIPELINE,
            "void PipelineCache::StartAsyncPipelineWorkers()",
            "\nvoid PipelineCache::StopAsyncPipelineWorkers",
        )
        self.assertIn(
            "if (!async_shader_retranslation_disabled)", worker_start
        )

    def test_admission_is_bounded_epoch_off_and_snapshot_first(self):
        for token in (
            "ExecutorHleCleanBufferMaxPages = 16",
            "FexWriteEpochs().enabled.load(std::memory_order_acquire)",
            "memory->AcquireSnapshotReadGuard()",
            "memory->ValidateSnapshotAccess",
            "EnterTrackedBulkWriteGate(address, size)",
        ):
            self.assertIn(token, PAGE if token.startswith("Executor") else PREPARE)
        self.assertLess(
            PREPARE.index("memory->AcquireSnapshotReadGuard()"),
            PREPARE.index("EnterTrackedBulkWriteGate(address, size)"),
        )

    def test_every_admitted_page_is_exactly_protected_buffer_watcher(self):
        page_validation = scope(
            PREPARE,
            "for (size_t page = first_page; page < page_end; ++page)",
            "\n            if (pages_valid)",
        )
        for token in (
            "!ActualWriteProtected(page)",
            "state->num_write_watchers != 1",
            "buffer_watchers != 1",
            "texture_watchers != 0",
            "WriteWatcherSourceMask(page) != 1",
        ):
            self.assertIn(token, page_validation)
        self.assertNotIn("bool protected_page", PREPARE)
        self.assertNotIn("else if (state->num_write_watchers != 0", PREPARE)

    def test_shader_writer_wraps_publish_and_contention_is_not_global_kill(self):
        self.assertLess(
            PREPARE.index("BeginHleCleanBufferWrite(page_begin, page_bytes)"),
            PREPARE.index("TryPublishHleCpuOwnedWrite(page_begin, page_bytes)"),
        )
        contention = scope(
            PREPARE,
            "if (!rasterizer->BeginHleCleanBufferWrite(page_begin, page_bytes))",
            "\n        ticket.pipeline_active = true;",
        )
        self.assertNotIn("hle_clean_buffer_fail_closed.store", contention)
        self.assertLess(
            contention.index("ReleaseHleCleanBufferWriteTicket(false)"),
            contention.index("InvalidateMemoryFromGuestThread"),
        )
        self.assertIn("return reject(ExecutorHleCleanBufferFallback::Shader)", contention)

    def test_shader_writer_is_permanently_try_only(self):
        self.assertIn("hle_clean_buffer_shader_scan_mutex.try_lock()", BEGIN_SHADER_WRITE)
        self.assertIn("return false;", BEGIN_SHADER_WRITE)
        self.assertNotIn("try_lock_for", BEGIN_SHADER_WRITE)
        self.assertNotIn("std::unique_lock", BEGIN_SHADER_WRITE)
        self.assertNotIn(".lock()", BEGIN_SHADER_WRITE)

    def test_postvalidate_unwinds_syncs_then_reenters_ordinary_prepare(self):
        body = scope(PREPARE, "if (!postvalidated)", "\n\n        const u64 active")
        self.assertLess(
            body.index("ReleaseHleCleanBufferWriteTicket(false)"),
            body.index("InvalidateMemoryFromGuestThread"),
        )
        self.assertIn("ExecutorHleCleanBufferFallback::Postvalidate", body)
        self.assertIn("return false;", body)

    def test_residency_underflow_poison_is_sticky_and_propagates_failure(self):
        publish = scope(PAGE, "void PublishHleResidencyHazard", "\n    bool HleResidencyHazard")
        poison = scope(PAGE, "void PoisonHleResidency", "\n    bool HleResidencyHazard")
        self.assertIn("hle_residency_poison", publish)
        self.assertIn("if (poisoned ||", publish)
        self.assertIn("hle_clean_buffer_fail_closed.store", poison)
        self.assertIn("hle_residency_poison", poison)
        self.assertIn("hle_residency_hazard", poison)
        for updater in ("UpdateTextureResidentCount", "UpdateShaderReadResidentCount"):
            body = scope(PAGE, f"bool {updater}", "\n    u32 " if updater.startswith("UpdateTexture") else "\n    bool GpuDirty")
            self.assertIn("PoisonHleResidency", body)
            self.assertIn("return false;", body)
        self.assertIn("return update_succeeded;", PAGE)

    def test_shader_retain_allocates_before_track_and_rolls_back(self):
        track = RETAIN.index("TrackHleShaderReadResidency(address, size)")
        for token in (
            "hle_shader_read_residency_leases.reserve",
            "hle_shader_read_residency_max_size.reserve",
            "hle_shader_read_residency_leases.emplace_back",
            "hle_shader_read_residency_max_size.insert_or_assign",
        ):
            self.assertLess(RETAIN.index(token), track)
        self.assertIn("rollback_staged_lease", RETAIN)
        self.assertIn("catch (...)" , RETAIN)
        self.assertIn("DisableHleCleanBufferFastPathFailClosed", RETAIN)
        self.assertNotIn("emplace_back", RETAIN[track:])
        self.assertNotIn("insert_or_assign", RETAIN[track:])

    def test_cached_unpinned_span_is_explicitly_failclosed_and_quiescent(self):
        cached = scope(GET_PARAMS, "const bool retained =", "\n                    return {")
        self.assertIn("if (!retained)", cached)
        self.assertIn("DisableHleCleanBufferFastPathFailClosed", cached)
        self.assertIn("IsHleCleanBufferReadStable", cached)
        self.assertIn("hle_clean_buffer_active_writers.load", cached)

    def test_marker_off_deferred_callback_has_no_lease_mutex_traffic(self):
        marker_off = scope(
            DOWNLOAD_ASYNC,
            "if (residency_lease == nullptr)",
            "\n        } else {",
        )
        self.assertIn("DeferPriorityOperation(std::move(write_backing))", marker_off)
        for token in ("lease_control", "scoped_lock", "running;"):
            self.assertNotIn(token, marker_off)

    def test_texture_registration_rollback_precedes_first_track(self):
        guard = REGISTER_IMAGE.index("SCOPE_EXIT")
        first_track = REGISTER_IMAGE.index("tracker.TrackTextureResidency")
        self.assertLess(guard, first_track)
        self.assertLess(
            first_track,
            REGISTER_IMAGE.index("image.flags |= ImageFlagBits::Registered"),
        )
        self.assertIn("UntrackImageMetaResidencies(image)", REGISTER_IMAGE)

    def test_mismatch_leave_syncs_original_ticket_range(self):
        leave = scope(
            PAGE,
            "bool TryLeaveHleCleanBufferWrite",
            "\n    bool TryPrepareHleCleanBufferWriteForMemset",
        )
        self.assertLess(leave.index("original_begin = ticket.page_begin"),
                        leave.index("ReleaseHleCleanBufferWriteTicket(false)"))
        self.assertIn("InvalidateMemoryFromGuestThread(original_begin", leave)
        self.assertIn("ReleaseDeferredWriteProtectionRange(original_begin", leave)

    def test_teardown_drains_writer_then_closes_before_untracking(self):
        destructor = scope(
            PIPELINE,
            "PipelineCache::~PipelineCache()",
            "\nstd::string PipelineCache::HostPipelineCacheName",
        )
        lock = destructor.index("std::unique_lock shader_scan_lock")
        disable = destructor.index("DisableHleCleanBufferFastPathFailClosed")
        untrack = destructor.index("UntrackHleShaderReadResidency")
        self.assertLess(lock, disable)
        self.assertLess(disable, untrack)

    def test_guest_read_scope_covers_full_graphics_and_compute_refresh(self):
        graphics = scope(
            PIPELINE,
            "bool PipelineCache::RefreshGraphicsStages",
            "\nbool PipelineCache::RefreshComputeKey",
        )
        compute = scope(
            PIPELINE,
            "bool PipelineCache::RefreshComputeKey",
            "\nvk::ShaderModule PipelineCache::CompileModule",
        )
        for body in (graphics, compute):
            admission = body.index(
                "HleCleanBufferGuestReadScope hle_guest_read_scope{*this}"
            )
            self.assertLess(admission, body.index("GetShaderParams("))
            self.assertLess(admission, body.index("GetProgram("))
            self.assertNotIn("hle_clean_buffer_shader_scan_mutex", body)
            self.assertNotIn("try_lock", body)

        self.assertIn("class HleCleanBufferGuestReadScope final", PIPELINE_HEADER)
        self.assertIn("~HleCleanBufferGuestReadScope()", PIPELINE_HEADER)

    def test_get_program_auxiliary_reads_remain_inside_reentrant_scope(self):
        admission = GET_PROGRAM.index(
            "HleCleanBufferGuestReadScope hle_guest_read_scope{*this}"
        )
        for token in (
            "BuildRuntimeInfo(stage, l_stage)",
            "RefreshFlatBuf(&srt_ranges, srt_memory_reader)",
            "ResolveFetchShader(*program, program->info)",
            "ResolveTessConstants(program->info)",
            "DoSrtSourceBytesMatch",
            "CaptureSrtBackingBytes",
            "ResolveFetchShader(*program, info)",
            "ResolveTessConstants(info)",
        ):
            self.assertLess(admission, GET_PROGRAM.index(token))

        params_admission = GET_PARAMS.index(
            "HleCleanBufferGuestReadScope hle_guest_read_scope{*this}"
        )
        self.assertLess(params_admission, GET_PARAMS.index("SearchBinaryInfo(code)"))
        self.assertNotIn("std::shared_lock shader_scan_lock", GET_PARAMS)

    def test_guest_read_scope_blocks_instead_of_skipping_draw(self):
        constructor = scope(
            PIPELINE,
            "HleCleanBufferGuestReadScope(const PipelineCache& owner_)",
            "\n\nPipelineCache::HleCleanBufferGuestReadScope::\n    ~HleCleanBufferGuestReadScope",
        )
        self.assertIn("while (!lock->try_lock_for", constructor)
        self.assertIn("action=wait-closed", constructor)
        self.assertNotIn("return false", constructor)
        self.assertIn("hle_clean_buffer_guest_read_depth", constructor)

    def test_counter_log_is_dense_then_bounded_periodic(self):
        maybe_log = scope(
            PAGE,
            "void ExecutorHleCleanBufferMaybeLog(const u64 ordinal)",
            "\nvoid ExecutorHleCleanBufferRecordFallback",
        )
        self.assertIn("ordinal > 64", maybe_log)
        self.assertIn("ordinal & 4095u", maybe_log)
        self.assertNotIn("ordinal - 1", maybe_log)

    def test_counters_expose_coverage_and_all_fallback_classes(self):
        for token in (
            "attempts=", "eligible=", "committed=", "avoidedSync=",
            "fallbackSize=", "fallbackAlias=", "fallbackMapping=",
            "fallbackPage=", "fallbackTexture=", "fallbackShader=",
            "fallbackRegionMissing=", "fallbackRegionBusy=",
            "fallbackRegionGpuDirty=", "fallbackPostvalidate=",
            "fallbackNested=", "pages=", "bytes=", "active=", "activeMax=",
        ):
            self.assertIn(token, PAGE)


if __name__ == "__main__":
    unittest.main()
