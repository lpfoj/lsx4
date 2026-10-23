"""Behavioral checks for bounded crash recovery after a probe transport failure."""

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from device_run_evidence import can_continue_after_transport_failure, inspect_stderr


class ProbeRunEvidenceTests(unittest.TestCase):
    run_id = "778d8387-36bc-4c72-87f3-b7a4c2a91f03"
    other_id = "32fc7d34-8591-44f7-a6d2-4f521c67def9"
    fatal = "[APPLE_UNHANDLED_SIGNAL] signal=0xb fault=0x38"

    def marker(self, run_id=None):
        return "[APPLE_PROBE_RUN] id=" + (run_id or self.run_id)

    def inspect(self, contents):
        return inspect_stderr(contents, self.run_id)

    def assert_no_fatal(self, contents, matched=False):
        self.assertEqual(self.inspect(contents), {
            "runId": self.run_id,
            "markerMatched": matched,
            "fatalDiagnostic": None,
        })

    def test_exact_current_marker_attributes_later_signal(self):
        self.assertEqual(self.inspect(self.marker() + "\n" + self.fatal + "\n"), {
            "runId": self.run_id,
            "markerMatched": True,
            "fatalDiagnostic": self.fatal,
        })

    def test_legacy_fatal_without_marker_cannot_prove_this_run(self):
        for contents in (self.fatal, "startup\n" + self.fatal + "\n", ""):
            with self.subTest(contents=contents):
                self.assert_no_fatal(contents)

    def test_fatal_before_matching_marker_is_stale(self):
        self.assert_no_fatal(self.fatal + "\n" + self.marker() + "\nstartup\n", True)

    def test_mismatched_run_marker_is_not_current_evidence(self):
        self.assert_no_fatal(self.marker(self.other_id) + "\n" + self.fatal)

    def test_later_run_marker_invalidates_earlier_matching_fatal(self):
        self.assert_no_fatal(self.marker() + "\n" + self.fatal + "\n"
                             + self.marker(self.other_id) + "\n")

    def test_latest_matching_run_ignores_earlier_runs_fatal(self):
        self.assert_no_fatal(self.marker(self.other_id) + "\n" + self.fatal
                             + "\n" + self.marker() + "\n", True)

    def test_repeated_matching_marker_resets_diagnostic_window(self):
        self.assert_no_fatal(self.marker() + "\n" + self.fatal + "\n"
                             + self.marker() + "\n", True)

    def test_marker_must_be_an_exact_complete_line(self):
        invalid = (
            "prefix " + self.marker(),
            " " + self.marker(),
            self.marker() + "suffix",
            self.marker() + " ",
            self.marker()[:-1],
            "[APPLE_PROBE_RUN] id=",
        )
        for marker in invalid:
            with self.subTest(marker=marker):
                self.assert_no_fatal(marker + "\n" + self.fatal)

    def test_later_malformed_marker_invalidates_old_crash(self):
        for malformed in ("[APPLE_PROBE_RUN] id=", self.marker() + "suffix"):
            with self.subTest(marker=malformed):
                self.assert_no_fatal(self.marker() + "\n" + self.fatal
                                     + "\n" + malformed + "\n")

    def test_windows_line_endings_preserve_exact_identity(self):
        evidence = self.inspect(self.marker() + "\r\n" + self.fatal + "\r\n")
        self.assertTrue(evidence["markerMatched"])
        self.assertEqual(evidence["fatalDiagnostic"], self.fatal)

    def test_terminal_patterns_keep_relevant_details(self):
        diagnostics = (
            "[APPLE_CRITICAL] signals.cpp:952 Unreachable code!\n"
            "Unhandled access violation: Read from address 0x38",
            "[APPLE_CRITICAL] memory.cpp:100 Assertion Failed!\n"
            "mapped range must remain owned",
            "libc++abi: terminating due to uncaught exception of type std::bad_alloc: std::bad_alloc",
            "memory allocation of 20 bytes failed",
            "memory allocation of 1048576 bytes failed",
        )
        for diagnostic in diagnostics:
            with self.subTest(diagnostic=diagnostic):
                evidence = self.inspect(self.marker() + "\n" + diagnostic + "\n")
                self.assertTrue(evidence["markerMatched"])
                self.assertEqual(evidence["fatalDiagnostic"], diagnostic)

    def test_nonfatal_allocation_and_other_logs_do_not_authorize_recovery(self):
        logs = (
            "[APPLE_ALLOCATION_FAILURE] footprint=2062796776 frames=13",
            "[APPLE_ALLOCATION_FRAME] index=2 address=0x103161e98",
            "[APPLE_CRITICAL] informational diagnostic without assertion",
            "prefix memory allocation of 20 bytes failed",
            "memory allocation of 20 bytes failed suffix",
            "[OTHER] Assertion or Unreachable in unrelated text",
        )
        for log in logs:
            with self.subTest(log=log):
                self.assert_no_fatal(self.marker() + "\n" + log + "\n", True)

    def test_termination_trap_does_not_replace_original_memory_fault(self):
        original = '[APPLE_UNHANDLED_SIGNAL] signal=0xb fault=0x8 pc=0x1050e6e28'
        termination = '[APPLE_UNHANDLED_SIGNAL] signal=0x5 fault=0x104073f94'
        evidence = self.inspect(self.marker() + '\n' + original
                                + '\n[APPLE_NATIVE_FAULT_FRAME] index=0 pc=0x1050e6e28'
                                + '\n' + termination + '\n')
        self.assertEqual(evidence['fatalDiagnostic'], original)
        self.assertEqual(evidence['fatalDiagnostics'], [original, termination])
        self.assertTrue(can_continue_after_transport_failure(True, None, evidence))

    def test_different_terminal_patterns_keep_log_order(self):
        worker = '[EXECUTOR_GUEST_THREAD_FEX] status=0 result=-5 name=Worker'
        assertion = '[APPLE_CRITICAL] signals.cpp:952 Unreachable code!\nUnhandled fault'
        evidence = self.inspect(self.marker() + '\n' + worker + '\n'
                                + self.fatal + '\n' + assertion + '\n')
        self.assertEqual(evidence['fatalDiagnostic'], worker)
        self.assertEqual(evidence['fatalDiagnostics'], [worker, self.fatal, assertion])

    def test_earlier_launch_cannot_contribute_to_fatal_chain(self):
        evidence = self.inspect(self.marker(self.other_id) + '\n' + self.fatal
                                + '\n' + self.marker() + '\n' + self.fatal)
        self.assertEqual(evidence['fatalDiagnostic'], self.fatal)
        self.assertNotIn('fatalDiagnostics', evidence)

    def test_failed_fex_worker_is_terminal_even_with_later_flips(self):
        diagnostic = '[LSX4Native] [EXECUTOR_GUEST_THREAD_FEX] status=-5 result=0 name=UnityPreload'
        evidence = self.inspect(self.marker() + '\n' + diagnostic
                                + '\n[APPLE_DEVICE_SAMPLE] {"flips":14000}\n')
        self.assertEqual(evidence['fatalDiagnostic'], diagnostic)
        self.assert_no_fatal(diagnostic + '\n' + self.marker() + '\n', True)

    def test_lost_metal_device_is_terminal_even_while_audio_continues(self):
        diagnostic = ('[mvk-error] VK_ERROR_OUT_OF_DEVICE_MEMORY: Lost VkDevice after '
                      'MTLCommandBuffer execution failed (code 9): Invalid Resource')
        evidence = self.inspect(self.marker() + '\n' + diagnostic
                                + '\n[APPLE_DEVICE_SAMPLE] {"audioFrames":22947840}\n')
        self.assertEqual(evidence['fatalDiagnostic'], diagnostic)
        self.assert_no_fatal(diagnostic + '\n' + self.marker(), True)
        self.assert_no_fatal(self.marker() + '\n[mvk-error] VK_ERROR_OUT_OF_DEVICE_MEMORY: '
                             'Could not allocate texture; fallback follows', True)

    def test_metal_debug_assertion_is_bound_to_current_launch(self):
        diagnostic = ('-[MTLDebugComputeCommandEncoder dispatchThreadgroups:threadsPerThreadgroup:]'
                      ':1377: failed assertion `workgroup count must not be 0.')
        self.assertEqual(self.inspect(self.marker() + '\n' + diagnostic)['fatalDiagnostic'], diagnostic)
        self.assert_no_fatal(diagnostic + '\n' + self.marker(), True)
        self.assert_no_fatal(self.marker() + '\n[OTHER] ' + diagnostic, True)

    def test_clean_workers_and_captured_signals_are_not_terminal(self):
        for line in (
            '[LSX4Native] [EXECUTOR_GUEST_THREAD_FEX] status=0 result=0 name=Worker',
            '[EXECUTOR_GUEST_THREAD_FEX] status=-0 result=0 name=Worker',
            '[EXECUTOR_GUEST_THREAD_FEX] status=0 result=-0 name=Worker',
            '[EXECUTOR_GUEST_THREAD_FEX] status=0 result=0 name=result=-5',
            '[LSX4Native] [EXECUTOR_FEX_signal] captured signum=11 generated=1',
        ):
            self.assert_no_fatal(self.marker() + '\n' + line + '\n', True)

    def test_metal_resource_identity_survives_multiline_assertion(self):
        header = ('-[MTLDebugDevice notifyExternalReferencesNonZeroOnDealloc:]'
                  ':3459: failed assertion `Resource still required:')
        diagnostic = (header + '\n<MTLToolsObject: 0x100> -> <Buffer: 0x200>'
                      '\n    label = Buffer 0x303824000:0x4000'
                      '\n    length = 16384'
                      "\n    storageMode = MTLStorageModePrivate'")
        for newline in ('\n', '\r\n'):
            evidence = self.inspect((self.marker() + '\n' + diagnostic
                                     + '\n' + self.fatal).replace('\n', newline))
            self.assertEqual(evidence['fatalDiagnostic'], diagnostic)
            self.assertEqual(evidence['fatalDiagnostics'], [diagnostic, self.fatal])
        self.assert_no_fatal(diagnostic + '\n' + self.marker(), True)

    def test_metal_continuation_is_bounded_and_cannot_swallow_events(self):
        header = '-[MTLDebugDevice dealloc:]:3459: failed assertion `Resource required:'
        evidence = self.inspect(self.marker() + '\n' + header + '\n'
                                + '    resource' * 1000 + '\n' + self.fatal)
        self.assertEqual(evidence['fatalDiagnostics'], [header, self.fatal])

    def test_metal_texture_view_assertion_preserves_incompatible_formats(self):
        header = '_mtlValidateArgumentsForTextureViewOnDevice:1866: failed assertion `Texture Creation'
        diagnostic = (header + '\nsource texture pixelFormat (MTLPixelFormatRGBA8Unorm) '
                      'not compatible with texture view pixelFormat (MTLPixelFormatRG8Unorm).\n\'')
        for newline in ('\n', '\r\n'):
            evidence = self.inspect((self.marker() + '\n' + diagnostic + '\n' + self.fatal).replace('\n', newline))
            self.assertEqual(evidence['fatalDiagnostics'], [diagnostic, self.fatal])
        self.assert_no_fatal(diagnostic + '\n' + self.marker(), True)
        self.assert_no_fatal(self.marker(self.other_id) + '\n' + diagnostic)
        evidence = self.inspect(self.marker() + '\n' + header + '\n[OTHER] next event\n' + self.fatal)
        self.assertEqual(evidence['fatalDiagnostics'], [header, self.fatal])
        evidence = self.inspect(self.marker() + '\n' + header + '\n[OTHER] next event'
                                + '\n    unrelated details\n' + self.fatal)
        self.assertEqual(evidence['fatalDiagnostics'], [header, self.fatal])

    def test_failed_callback_result_is_terminal_with_successful_api_status(self):
        # The native bridge rejects either API status or callback_result.status.
        # A successful dispatch can still report a failed guest callback.
        for prefix in ('', '[LSX4Native] '):
            for status in ('0', '-0'):
                diagnostic = (prefix + '[EXECUTOR_GUEST_THREAD_FEX] status='
                              + status + ' result=-5 start=0x300000000 name=Worker')
                with self.subTest(diagnostic=diagnostic):
                    evidence = self.inspect(self.marker() + '\n' + diagnostic
                                            + '\n[APPLE_DEVICE_SAMPLE] {"flips":14000}\n')
                    self.assertEqual(evidence['fatalDiagnostic'], diagnostic)

    def test_old_callback_failure_cannot_fail_a_new_launch(self):
        diagnostic = '[EXECUTOR_GUEST_THREAD_FEX] status=0 result=-5 name=Worker'
        self.assert_no_fatal(diagnostic + '\n' + self.marker() + '\n', True)
        self.assert_no_fatal(self.marker(self.other_id) + '\n' + diagnostic)

    def test_missing_expected_identity_never_accepts_log(self):
        for expected in (None, ""):
            with self.subTest(expected=expected):
                evidence = inspect_stderr(self.marker() + "\n" + self.fatal, expected)
                self.assertFalse(evidence["markerMatched"])
                self.assertIsNone(evidence["fatalDiagnostic"])

    def test_confirmed_awake_and_current_terminal_diagnostic_can_continue(self):
        evidence = self.inspect(self.marker() + "\n" + self.fatal)
        self.assertTrue(can_continue_after_transport_failure(True, None, evidence))

    def test_recovered_awake_alone_is_not_fatal_run_evidence(self):
        contents = (
            "", self.fatal, self.marker(),
            self.marker(self.other_id) + "\n" + self.fatal,
            self.marker() + "\n[APPLE_ALLOCATION_FAILURE] frames=13",
            self.fatal + "\n" + self.marker(),
        )
        for log in contents:
            with self.subTest(log=log):
                self.assertFalse(can_continue_after_transport_failure(True, None, self.inspect(log)))

    def test_fresh_return_route_remains_available_without_stderr_marker(self):
        # The caller already checked this record against the launch timestamp.
        fresh_return = {"timestamp": 1788715000, "returned": True}
        self.assertTrue(can_continue_after_transport_failure(True, fresh_return, self.inspect("")))

    def test_awake_failure_blocks_both_recovery_routes(self):
        evidence = self.inspect(self.marker() + "\n" + self.fatal)
        for returned in (None, {"timestamp": 1788715000, "returned": True}):
            with self.subTest(returned=returned):
                self.assertFalse(can_continue_after_transport_failure(False, returned, evidence))


if __name__ == "__main__":
    unittest.main()
