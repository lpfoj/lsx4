"""Exercise host LLDB event handling using a local fake peer on a real PTY."""
import contextlib
import importlib.util
import io
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location(
    "device_cycle", Path(__file__).resolve().parents[2] / "device_cycle.py")
cycle = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cycle)


PEER = r'''
import sys,time
mode=sys.argv[1]
sys.stdout.write('(lldb) ');sys.stdout.flush()
for command in sys.stdin:
    command=command.strip()
    if command.startswith('device select '):
        if mode=='lookup':print('Core Device lookup timed out.',flush=True)
        elif mode=='locked':print('Device needs to be unlocked',flush=True)
        sys.stdout.write('(lldb) ');sys.stdout.flush()
    elif command.startswith('device process attach '):
        print('Process 42 stopped',flush=True)
        sys.stdout.write('(lldb) ');sys.stdout.flush()
    elif command=='continue':
        if mode=='lookup-after-attach':
            print('Core Device lookup timed out.',flush=True)
            sys.stdout.write('(lldb) ');sys.stdout.flush()
            continue
        if mode=='timeout':
            while True:
                print('LSX4_JIT_PROGRESS {"completedBytes":16777216}',flush=True)
                time.sleep(0.02)
        sys.stdout.write('LSX4_JIT_PRO');sys.stdout.flush();time.sleep(0.02)
        sys.stdout.write('GRESS {"completedBytes":16777216}\n');sys.stdout.flush()
        if mode=='failure':
            print('error: Failed to resume process: fake transport failure',flush=True)
        else:
            print('LSX4_TEST_READY',flush=True)
        sys.stdout.write('(lldb) ');sys.stdout.flush()
    elif command=='process detach':
        # Match the actual asynchronous LLDB ordering: prompt before event.
        sys.stdout.write('(lldb) Process 42 detached\n');sys.stdout.flush()
    elif command=='quit':
        break
'''


class DeviceProgressRelayTests(unittest.TestCase):
    def test_split_progress_line_is_forwarded_once_without_other_output(self):
        lines = []
        relay = cycle.DebuggerProgressRelay(lines.append)
        relay.feed('ordinary output\r\nLSX4_JIT_PRO')
        relay.feed('GRESS {"completedBytes":16}')
        self.assertEqual(lines, [])
        relay.feed('\r\n(lldb) continue\r\nLSX4_JIT_PROGRESS {"completedBytes":32}\n')
        self.assertEqual(lines, [
            'LSX4_JIT_PROGRESS {"completedBytes":16}',
            'LSX4_JIT_PROGRESS {"completedBytes":32}',
        ])

    def run_peer(self, mode, timeout):
        actual_popen = subprocess.Popen
        def launch(_args, **kwargs):
            return actual_popen([sys.executable, '-u', '-c', PEER, mode], **kwargs)
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'lldb.log'
            output = io.StringIO()
            with patch.object(cycle.subprocess, 'Popen', side_effect=launch):
                with contextlib.redirect_stdout(output):
                    cycle.debug_device(['continue', 'process detach', 'quit'], path,
                                       timeout, ready_pattern='LSX4_TEST_READY')
            return output.getvalue(), path.read_text()

    def test_real_pty_fragmented_progress_preserves_ready_and_detach(self):
        output, log = self.run_peer('ready', 2)
        self.assertEqual(output.count('LSX4_JIT_PROGRESS '), 1)
        self.assertIn('LSX4_TEST_READY', log)
        self.assertIn('Process 42 detached', log)

    def test_progress_never_extends_the_hard_ready_deadline(self):
        started = time.monotonic()
        with self.assertRaises(TimeoutError):
            self.run_peer('timeout', 0.2)
        self.assertLess(time.monotonic() - started, 3)

    def test_progress_never_masks_a_transport_failure(self):
        with self.assertRaisesRegex(RuntimeError, 'transport failed while resuming'):
            self.run_peer('failure', 2)

    def run_lookup_peers(self, modes, expected_error=None, recover_pre_attach=None):
        actual_popen = subprocess.Popen
        launches = []
        def launch(_args, **kwargs):
            mode = modes[len(launches)]
            launches.append(mode)
            return actual_popen([sys.executable, '-u', '-c', PEER, mode], **kwargs)
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'lldb.log'
            with patch.object(cycle.subprocess, 'Popen', side_effect=launch):
                with contextlib.redirect_stdout(io.StringIO()):
                    context = self.assertRaises(expected_error) if expected_error else contextlib.nullcontext()
                    with context:
                        cycle.debug_device(['device select physical-device', 'device process attach -p 42',
                            'continue', 'process detach', 'quit'], path, 2, ready_pattern='LSX4_TEST_READY',
                            recover_pre_attach=recover_pre_attach)
            logs = {p.name: p.read_text() for p in Path(folder).glob('*.log')}
            self.assertEqual(launches, modes)
            return logs

    def test_discovery_retry_preserves_failures_and_attaches_only_once(self):
        logs = self.run_lookup_peers(['lookup', 'lookup', 'ready'])
        self.assertEqual(len(logs), 3)
        for name in ['lldb.lookup-1.log', 'lldb.lookup-2.log']:
            self.assertIn('Core Device lookup timed out', logs[name])
            self.assertNotIn('device process attach', logs[name])
        self.assertIn('LSX4_TEST_READY', logs['lldb.log'])

    def test_discovery_retry_has_a_finite_bound(self):
        logs = self.run_lookup_peers(['lookup'] * 3, cycle.PreAttachDeviceLookupTimeout)
        self.assertEqual(len(logs), 3)

    def test_lookup_error_after_attach_never_replays_execution(self):
        self.run_lookup_peers(['lookup-after-attach'], RuntimeError)

    def test_locked_device_does_not_trigger_discovery_retry(self):
        self.run_lookup_peers(['locked'], RuntimeError)

    def test_discovery_recovery_runs_once_before_final_attempt(self):
        recoveries=[]
        self.run_lookup_peers(['lookup','lookup','ready'],
                              recover_pre_attach=lambda:recoveries.append(True))
        self.assertEqual(recoveries,[True])

    def test_failed_recovery_does_not_add_unbounded_discovery_attempts(self):
        recoveries=[]
        self.run_lookup_peers(['lookup']*3,cycle.PreAttachDeviceLookupTimeout,
                              recover_pre_attach=lambda:recoveries.append(True))
        self.assertEqual(recoveries,[True])

    def test_recovery_never_runs_for_attached_or_locked_or_healthy_device(self):
        recoveries=[]
        for modes,error in [(['lookup-after-attach'],RuntimeError),
                            (['locked'],RuntimeError),(['ready'],None),
                            (['lookup','ready'],None)]:
            self.run_lookup_peers(modes,error,
                                  recover_pre_attach=lambda:recoveries.append(True))
        self.assertEqual(recoveries,[])


if __name__ == '__main__':
    unittest.main()
