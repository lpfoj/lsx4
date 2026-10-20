import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "src/executor/native_runtime_api.cpp").read_text(encoding="utf-8")


def section(source: str, start: str, end: str) -> str:
    return source.split(start, 1)[1].split(end, 1)[0]


class FexHleThunkAdmissionContractTests(unittest.TestCase):
    def test_diagnostic_is_explicitly_marker_only(self):
        self.assertIn("run-diag-fex-hle-thunk-admission", SOURCE)
        self.assertIn("EXECUTOR_DIAG_FEX_HLE_THUNK_ADMISSION", SOURCE)
        self.assertIn("diagnosticOnly=1 fpsVerdictAllowed=0", SOURCE)

    def test_production_off_path_returns_before_tls_or_lock(self):
        record = section(
            SOURCE,
            "void RecordFexHleThunkAdmission(",
            "static std::uint64_t ExecutorMappedHleBridgeCallbackCommon",
        )
        disabled = record.index("if (!FexHleThunkAdmissionEnabled())")
        early_return = record.index("return;", disabled)
        tls = record.index("static thread_local FexHleThunkAdmissionThreadState")
        flush = record.index("FlushFexHleThunkAdmission(local)")
        self.assertLess(disabled, early_return)
        self.assertLess(early_return, tls)
        self.assertLess(tls, flush)
        self.assertNotIn("fetch_add", record)
        self.assertNotIn("atomic_thread_fence", record)

    def test_abi_shape_is_fail_closed_and_not_called_safe(self):
        shape = section(
            SOURCE,
            "bool IsFexHleIntegerRegisterShape(",
            "std::uint32_t FexHleThunkAdmissionRiskMask",
        )
        for gate in (
            "descriptor.supported",
            "descriptor.stack_argument_bytes != 0",
            "descriptor.sse_argument_count != 0",
            "descriptor.sret_flags != 0",
            "descriptor.call_flags != 0",
            "descriptor.return_eightbyte_count > 1",
            "AndroidX64SysVClass::Integer",
        ):
            self.assertIn(gate, shape)
        self.assertIn("source_audit_candidate", SOURCE)
        self.assertNotIn("typed_safe", shape)

    def test_semantic_risks_exclude_existing_broad_leaf_categories(self):
        risk = section(
            SOURCE,
            "std::uint32_t FexHleThunkAdmissionRiskMask(",
            "void FlushFexHleThunkAdmission",
        )
        for gate in (
            "metadata.pthread_target",
            "metadata.blocking_sem_wait_target",
            "metadata.posix_sem_site_op",
            "metadata.fex_guest_thread_exit_target",
            "metadata.fex_context_replacing_fiber_target",
            "metadata.system_service_load_exec_target",
            "metadata.audio_out_target",
            "metadata.gnm_submit_and_flip",
            "known_pointer_semantics",
            'HleNameMatches(name, "posix_gettimeofday", "n88vx3C5nW8")',
            'HleNameMatches(name, "memcpy", "Q3VBxCXhUHs")',
            'SymbolNameContains(symbol_name, "ExecutorLibcMemset")',
            "std::count(descriptor.signature",
            "metadata.pointer_result_family",
            "metadata.name.empty() && metadata.symbol_name.empty()",
        ):
            self.assertIn(gate, risk)
        self.assertNotIn("!metadata.known_target", risk)

    def test_dispatch_records_after_descriptor_and_metadata_resolution(self):
        dispatch = section(
            SOURCE,
            'extern "C" int32_t ExecutorFexDispatchHle',
            "void ExecutorFexThreadEvent",
        )
        descriptor = dispatch.index("ResolveAndroidX64HleAbiDescriptor(")
        metadata = dispatch.index("GetExecutorHleTargetMetadata(")
        admission = dispatch.index("RecordFexHleThunkAdmission(")
        invoke = dispatch.index("InvokeAndroidX64HleTypedTargetResolved(")
        self.assertLess(descriptor, metadata)
        self.assertLess(metadata, admission)
        self.assertLess(admission, invoke)

    def test_committed_histogram_reports_integrity_and_scene_independent_rate(self):
        for field in (
            "callIdSpan=",
            "committedPercent=",
            "boundaryRate=",
            "integerRegisterShapePercent=",
            "sourceAuditCandidatePercent=",
            "targetOverflowCalls=",
            "callIdRegressions=",
            "exactCommittedTargets=",
        ):
            self.assertIn(field, SOURCE)


if __name__ == "__main__":
    unittest.main()
