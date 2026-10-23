"""Exercise the real LLDB callback without attaching to a process or device."""
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import sys
from types import SimpleNamespace
import unittest
from unittest.mock import patch


MIB = 1024 * 1024
SLAB = 4 * MIB
ADDRESS = 0x100000000
ROOT = Path(__file__).resolve().parents[2]


def load_module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class FakeError:
    def __init__(self):
        self.failed = False

    def Fail(self):
        return self.failed

    def __str__(self):
        return "injected LLDB write failure" if self.failed else "success"


class FakeProcess:
    def __init__(self, triple="arm64-apple-ios", responses=()):
        self.triple = triple
        self.responses = iter(responses)
        self.writes = []

    def GetTarget(self):
        return SimpleNamespace(GetTriple=lambda: self.triple)

    def WriteMemory(self, address, payload, error):
        self.writes.append((address, payload))
        written, failed = next(self.responses, (len(payload), False))
        error.failed = failed
        return written


def frame_for(process, address=ADDRESS, size=SLAB, module="LSX4RuntimeProbe"):
    registers = {"x0": address, "x1": size}
    return SimpleNamespace(
        GetThread=lambda: SimpleNamespace(GetProcess=lambda: process),
        FindRegister=lambda name: SimpleNamespace(
            GetValueAsUnsigned=lambda: registers[name]),
        GetModule=lambda: SimpleNamespace(GetFileSpec=lambda: SimpleNamespace(
            GetFilename=lambda: module)),
    )


class JitPreparationTests(unittest.TestCase):
    def setUp(self):
        # Import executes no LLDB initialization hook. Every test gets fresh
        # callback state and a fake SBError; there is no real debugger API.
        with patch.dict(sys.modules, {"lldb": SimpleNamespace(SBError=FakeError)}):
            self.jit = load_module("jit_preparation_under_test", ROOT / "Debug/lsx4_jit.py")

    def prepare(self, process, **kwargs):
        with contextlib.redirect_stdout(io.StringIO()) as output:
            result = self.jit.prepare_pages(frame_for(process, **kwargs), None, None)
        self.assertIs(result, False, "callback must always auto-continue")
        return output.getvalue()

    def test_full_slab_is_zeroed_before_same_address_acknowledgement(self):
        self.jit.configure_progress(SLAB)
        process = FakeProcess()
        output = self.prepare(process)
        self.assertEqual(process.writes, [(ADDRESS, bytes(SLAB)), (ADDRESS, b"LSX4JIT!")])
        progress = json.loads(output.removeprefix("LSX4_JIT_PROGRESS "))
        self.assertEqual(progress["completedBytes"], SLAB)
        self.assertEqual(progress["completedSlabs"], 1)
        self.assertEqual(self.jit.statistics()["pagePreparationCalls"], 1)

    def test_partial_or_failed_zero_write_never_acknowledges_or_advances(self):
        for response in [(SLAB - 1, False), (0, True), (SLAB, True)]:
            with self.subTest(response=response):
                self.jit.configure_progress(SLAB)
                process = FakeProcess(responses=[response])
                output = self.prepare(process)
                self.assertEqual(process.writes, [(ADDRESS, bytes(SLAB))])
                self.assertEqual(self.jit._progress["completedBytes"], 0)
                self.assertEqual(self.jit._progress["completedSlabs"], 0)
                self.assertNotIn("LSX4_JIT_PROGRESS ", output)

    def test_partial_or_failed_ack_never_advances_and_retry_can_complete(self):
        for response in [(7, False), (0, True), (8, True)]:
            with self.subTest(response=response):
                self.jit.configure_progress(SLAB)
                process = FakeProcess(responses=[(SLAB, False), response])
                output = self.prepare(process)
                self.assertEqual(process.writes, [(ADDRESS, bytes(SLAB)), (ADDRESS, b"LSX4JIT!")])
                self.assertEqual(self.jit._progress["completedBytes"], 0)
                self.assertEqual(self.jit._progress["completedSlabs"], 0)
                self.assertNotIn("LSX4_JIT_PROGRESS ", output)
                self.prepare(FakeProcess())
                self.assertEqual(self.jit._progress["completedBytes"], SLAB)
                self.assertEqual(self.jit._progress["completedSlabs"], 1)

    def test_invalid_request_never_writes_even_with_configured_progress(self):
        requests = [
            {"module": "unrelated.dylib"}, {"address": 0}, {"address": ADDRESS + 4},
            {"size": 0}, {"size": 16383}, {"size": SLAB + 16384},
        ]
        self.jit.configure_progress(SLAB)
        for request in requests:
            with self.subTest(request=request):
                process = FakeProcess()
                self.prepare(process, **request)
                self.assertEqual(process.writes, [])
        process = FakeProcess(triple="arm64-apple-macosx")
        self.prepare(process)
        self.assertEqual(process.writes, [])
        self.assertEqual(self.jit._progress["completedBytes"], 0)

    def test_each_allowed_app_module_can_prepare_one_page(self):
        for module in ("LSX4iOS", "LSX4iOS.debug.dylib", "LSX4RuntimeProbe",
                       "LSX4RuntimeProbe.debug.dylib"):
            with self.subTest(module=module):
                process = FakeProcess()
                self.prepare(process, module=module, size=16384)
                self.assertEqual(process.writes, [(ADDRESS, bytes(16384)), (ADDRESS, b"LSX4JIT!")])

    def test_unconfigured_progress_does_not_change_preparation_or_emit(self):
        for _ in range(4):
            process = FakeProcess()
            output = self.prepare(process, size=16384)
            self.assertEqual(len(process.writes), 2)
            self.assertNotIn("LSX4_JIT_PROGRESS ", output)

    def test_progress_reports_every_four_successes_and_final_with_flush(self):
        self.jit.configure_progress(5 * SLAB)
        with patch("builtins.print") as printer:
            for index in range(5):
                self.assertIs(self.jit.prepare_pages(
                    frame_for(FakeProcess(), address=ADDRESS + index * SLAB), None, None), False)
                self.assertEqual(printer.call_count, 0 if index < 3 else index - 2)
        reports = []
        for call in printer.call_args_list:
            self.assertEqual(call.kwargs, {"flush": True})
            self.assertTrue(call.args[0].startswith("LSX4_JIT_PROGRESS "))
            reports.append(json.loads(call.args[0].removeprefix("LSX4_JIT_PROGRESS ")))
        self.assertEqual([report["completedBytes"] for report in reports], [4 * SLAB, 5 * SLAB])
        self.assertEqual([report["completedSlabs"] for report in reports], [4, 5])
        for report in reports:
            self.assertEqual(report["expectedBytes"], 5 * SLAB)
            self.assertGreaterEqual(report["callbackWallSeconds"], 0)
        self.assertGreaterEqual(reports[1]["callbackWallSeconds"], reports[0]["callbackWallSeconds"])

    def test_progress_validation_and_reconfiguration(self):
        self.jit.configure_progress(SLAB)
        self.prepare(FakeProcess())
        for invalid in (None, "4194304", True, 0, SLAB - 1, SLAB + 16384, 516 * MIB, float(SLAB)):
            with self.subTest(invalid=invalid):
                with self.assertRaises(ValueError):
                    self.jit.configure_progress(invalid)
                self.assertEqual(self.jit._progress["completedBytes"], SLAB)
        self.jit.configure_progress(512 * MIB)
        self.assertEqual(self.jit._progress,
                         {"expectedBytes": 512 * MIB, "completedBytes": 0, "completedSlabs": 0})

    def test_statistics_are_recorded_when_transport_raises(self):
        process = FakeProcess()
        with patch.object(process, "WriteMemory", side_effect=RuntimeError("transport lost")):
            with self.assertRaisesRegex(RuntimeError, "transport lost"):
                self.jit.prepare_pages(frame_for(process), None, None)
        self.assertEqual(self.jit.statistics()["pagePreparationCalls"], 1)
        self.assertGreaterEqual(self.jit.statistics()["callbackWallSeconds"], 0)
        self.assertEqual(self.jit._progress["completedBytes"], 0)


policy = load_module("startup_policy_under_test", ROOT / "RuntimeCore/device_startup_policy.py")


class StartupDeadlineTests(unittest.TestCase):
    def test_prepared_pool_budgets_are_finite_and_outer_covers_inner(self):
        for pool, expected in [(4, 95), (64, 170), (128, 250), (256, 410), (512, 730)]:
            for seconds in (5, 20, 600):
                with self.subTest(pool=pool, seconds=seconds):
                    ready = policy.debugger_ready_timeout(seconds, pool)
                    outer = policy.device_probe_timeout(seconds, pool)
                    self.assertEqual(ready, expected)
                    self.assertEqual(outer, expected + seconds + 30 + 360)
                    self.assertLess(ready + seconds + 30, outer)

    def test_no_pool_retains_observation_plus_ninety_seconds(self):
        for seconds in (5, 20, 600):
            self.assertEqual(policy.debugger_ready_timeout(seconds), seconds + 90)
            self.assertEqual(policy.device_probe_timeout(seconds), 2 * seconds + 480)

    def test_invalid_pool_or_observation_cannot_create_a_deadline(self):
        for function in (policy.debugger_ready_timeout, policy.device_probe_timeout):
            for pool in (0, 2, 6, 63, 258, 514, 516):
                with self.subTest(function=function.__name__, pool=pool):
                    with self.assertRaises(ValueError):
                        function(20, pool)
            for seconds in (0, 4, 601):
                with self.subTest(function=function.__name__, seconds=seconds):
                    with self.assertRaises(ValueError):
                        function(seconds, 64)


if __name__ == "__main__":
    unittest.main()
