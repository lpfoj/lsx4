from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
STUBS = (ROOT / "funnel-arm/src/core/aerolib/stubs.cpp").read_text(encoding="utf-8")
FIBER = (
    ROOT / "funnel-arm/src/core/libraries/fiber/fiber.cpp"
).read_text(encoding="utf-8")


def between(source: str, start: str, end: str) -> str:
    return source.split(start, 1)[1].split(end, 1)[0]


def test_old_title_specific_libc_write_watch_is_absent_from_copy_hot_paths():
    assert "ExecutorWatchGateWrite" not in STUBS
    assert "[EXECUTOR_GATE_WRITE]" not in STUBS
    assert "[EXECUTOR_STRCPY_NAME]" not in STUBS
    for signature, end in (
        ("u64 ExecutorLibcMemcpy(", "u64 ExecutorLibcMemmove("),
        ("u64 ExecutorLibcMemmove(", "std::atomic<int> g_memset_guard_log"),
        ("u64 ExecutorLibcMemset(", "u64 ExecutorLibcMemchr("),
    ):
        body = between(STUBS, signature, end)
        assert "PrepareBulkGuestWrite" in body
        assert "BeginBulkGuestWrite" in body
        assert "EndBulkGuestWrite" in body


def test_memcmp_preview_reads_and_sampling_are_verbose_only():
    body = between(STUBS, "s64 ExecutorLibcMemcmp(", "executor_lsx4_android_run_guest_signal_handler")
    gate = body.index("const bool trace_memcmp = ExecutorVerboseLibcStringLogsEnabled()")
    first_preview_copy = body.index("std::memcpy(lhs_preview_bytes.data()")
    preview_gate = body.rfind("if (trace_memcmp)", 0, first_preview_copy)
    assert gate < preview_gate < first_preview_copy
    low_sample = body.index("s_low_memcmp_budget.fetch_sub")
    low_gate = body.rfind("if (trace_memcmp &&", 0, low_sample)
    assert low_gate != -1
    assert low_sample - low_gate < 220


def test_fiber_transition_capture_is_positive_diagnostic_only():
    gate = between(
        FIBER,
        "bool AndroidFiberTransitionDiagnosticsEnabled()",
        "bool HasPersistentAndroidFiberContext",
    )
    assert "EXECUTOR_DIAG_FIBER_TRANSITIONS" in gate
    assert 'std::strcmp(value, "0") != 0' in gate

    trace = between(
        FIBER,
        "void TraceAndroidFiberTransition(",
        "void DumpRecentAndroidFiberTransitions(",
    )
    early_return = trace.index("if (!AndroidFiberTransitionDiagnosticsEnabled())")
    assert early_return < trace.index("PublishedLiveState()")
    assert early_return < trace.index("transition_sequence++")
    assert early_return < trace.index("recent_transitions[")


def test_fiber_errors_remain_unconditional_but_success_budget_is_gated():
    trace = between(
        FIBER,
        "s32 TraceAndroidFiberResult(",
        "\n}\n\n}\n#endif",
    )
    error = trace.index("if (result != ORBIS_OK && !expected_get_self_probe)")
    gate = trace.index("if (!AndroidFiberTransitionDiagnosticsEnabled())")
    budget = trace.index("g_android_fiber_trace_budget.load")
    assert error < gate < budget
    assert "[LSX4_FIBER_API_ERROR]" in trace[:gate]
    assert "[LSX4_FIBER_API]" in trace[gate:]


def test_successful_allocator_stderr_is_positive_gated_on_android():
    functions = (
        ("u64 ExecutorMspaceRealloc(", "u64 ExecutorMspaceReallocalign("),
        ("u64 ExecutorMspaceReallocalign(", "u64 ExecutorMspaceLock("),
        ("u64 ExecutorMspacePosixMemalign(", "u64 ExecutorMspaceMallocUsableSize("),
        ("u64 ExecutorMspaceMallocUsableSize(", "u64 ExecutorMspaceNoopStats("),
        ("u64 ExecutorLibcRealloc(", "u64 ExecutorLibcReallocalign("),
        ("u64 ExecutorLibcReallocalign(", "u64 ExecutorLibcFree("),
        ("u64 ExecutorLibcPosixMemalign(", "u64 ExecutorLibcMallocUsableSize("),
        ("u64 ExecutorLibcMallocUsableSize(", "static u64 ExecutorUntagHostPointer("),
        ("u64 ExecutorPngMalloc(", "u64 ExecutorPngCalloc("),
        ("u64 ExecutorPngCalloc(", "u64 ExecutorPngFree("),
        ("u64 ExecutorPngFree(", "u64 ExecutorRaise("),
    )
    for start, end in functions:
        body = between(STUBS, start, end)
        output = body.index("std::fprintf(stderr")
        gate = body.rfind("ExecutorVerboseAerolibAllocLogsEnabled()", 0, output)
        assert gate != -1, start
        assert output - gate < 110, start


def test_getenv_file_and_logcat_are_io_diagnostic_only():
    body = between(STUBS, "u64 ExecutorLibcGetenv(", "s64 ExecutorLibcSetenv(")
    trace = body.index("const bool trace_env = ExecutorVerboseAerolibIoLogsEnabled()")
    file_open = body.index('executor-getenv.log')
    assert trace < body.rfind("if (trace_env)", 0, file_open) < file_open
    null_log = body.index("[EXECUTOR_LIBC_GETENV] name=%s result=<null>")
    assert body.rfind("if (trace_env)", 0, null_log) > trace
    value_log = body.index("[EXECUTOR_LIBC_GETENV] name=%s value=%s")
    assert "if (trace_env && log_budget.fetch_sub" in body[trace:value_log]


def test_directory_and_syslock_output_are_positive_gated():
    directory = between(STUBS, "u64 ExecutorOpendir(", "struct ExecutorBgftDownloadParam")
    cursor = 0
    while True:
        output = directory.find("std::fprintf(stderr", cursor)
        if output == -1:
            break
        gate = directory.rfind("ExecutorVerboseAerolibIoLogsEnabled()", 0, output)
        assert gate != -1
        assert output - gate < 180
        cursor = output + 1

    for start, end, operation in (
        ("ExecutorLibcLocksyslock(int lock)", "ExecutorLibcUnlocksyslock(int lock)", ".lock()"),
        ("ExecutorLibcUnlocksyslock(int lock)", "ExecutorCxaAtexit", ".unlock()"),
    ):
        body = between(STUBS, start, end)
        gate = body.index("if (ExecutorVerboseAerolibSyslockLogsEnabled())")
        assert body.index(operation) < gate
        assert gate < body.index("log_budget.fetch_sub")


def test_non_mono_interposer_arguments_do_not_arm_malloc_hook_polling():
    body = between(STUBS, "u64 ExecutorMonoPathLog(", "ExecutorMonoDomainOpenLog(")
    validation = body.index("looks_like_mono_path")
    rejection = body.index("if (!looks_like_mono_path(buf)")
    arm = body.index("g_mono_loadhook_ready.store")
    assert "Bloodborne has passed 0xaf" in body
    assert validation < rejection < arm
    assert body.index("ExecutorReadGuestCStr(path_ptr") < rejection
    assert "Managed" in body[:arm]
