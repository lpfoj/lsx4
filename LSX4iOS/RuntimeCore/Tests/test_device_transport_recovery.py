import sys
from pathlib import Path
import unittest
from unittest.mock import patch
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import device_transport_recovery as recovery

class TransportRecoveryContract(unittest.TestCase):
    def test_only_exact_service_owned_by_current_user_is_stopped(self):
        listing = '\n'.join(['10 0 '+recovery.SERVICE, '11 501 '+recovery.SERVICE,
                             '12 501 /tmp/CoreDeviceService', '13 501 /usr/bin/adb',
                             '14 501 /Applications/Xcode.app/CoreSimulatorService', 'bad entry'])
        with patch.object(recovery.subprocess, 'check_output', return_value=listing), \
             patch.object(recovery.os, 'getuid', return_value=501), \
             patch.object(recovery.os, 'kill') as kill, \
             patch.object(recovery.time, 'sleep'):
            self.assertEqual(recovery.restart_user_coredevice(), [11])
            kill.assert_called_once_with(11, recovery.signal.SIGTERM)

    def test_already_exited_service_does_not_abort_transfer_retry(self):
        with patch.object(recovery.subprocess, 'check_output', return_value='11 501 '+recovery.SERVICE), \
             patch.object(recovery.os, 'getuid', return_value=501), \
             patch.object(recovery.os, 'kill', side_effect=ProcessLookupError), \
             patch.object(recovery.time, 'sleep') as sleep:
            self.assertEqual(recovery.restart_user_coredevice(), [])
            sleep.assert_not_called()


class ProbeTransferRetryContract(unittest.TestCase):
    def setUp(self):
        import ast, json, tempfile, textwrap
        from types import SimpleNamespace
        from unittest.mock import Mock
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        source = (Path(__file__).resolve().parents[1]/'device_probe.py').read_text()
        main = next(n for n in ast.parse(source).body if isinstance(n,ast.FunctionDef) and n.name=='main')
        function = next(n for n in main.body if isinstance(n,ast.FunctionDef) and n.name=='device')
        self.restart = Mock(return_value=[42])
        namespace = {'json':json,'subprocess':recovery.subprocess,'restart_user_coredevice':self.restart}
        factory = 'def create(args, observation_active):\n    file_transport_recovered=False\n'+textwrap.indent(ast.unparse(function),'    ')+'\n    return device\n'
        exec(factory, namespace)
        self.create = lambda live: namespace['create'](SimpleNamespace(output=self.root,device='test-device',live_query_timeout=8),live)
        self.calls = 0
        def failing_once(command, *, stdout, **kwargs):
            self.calls += 1
            if self.calls==1:
                stdout.write('ERROR: Command timeout of 25.0 seconds exceeded.\n');stdout.flush()
                raise recovery.subprocess.CalledProcessError(2,command)
            Path(command[-1]).write_text(json.dumps({'result':{'ok':True}}))
        self.run = failing_once

    def test_idempotent_copy_timeout_recovers_once_between_observations(self):
        import json
        call=self.create(False)
        with patch.object(recovery.subprocess,'run',side_effect=self.run):
            self.assertEqual(call(['copy','to','--source','config'],'profile'),{'ok':True})
            self.calls=0
            with self.assertRaises(recovery.subprocess.CalledProcessError):
                call(['copy','from','--source','report'],'report')
        self.restart.assert_called_once()
        note=json.loads((self.root/'file-transport-recovery.json').read_text())
        self.assertFalse(note['duringObservation']);self.assertFalse(note['launchReplayed'])

    def test_live_observation_timeout_never_restarts_transport(self):
        with patch.object(recovery.subprocess,'run',side_effect=self.run):
            with self.assertRaises(recovery.subprocess.CalledProcessError):
                self.create(True)(['copy','from','--source','report'],'live')
        self.restart.assert_not_called();self.assertEqual(self.calls,1)

    def test_uncertain_process_launch_is_never_replayed(self):
        with patch.object(recovery.subprocess,'run',side_effect=self.run):
            with self.assertRaises(recovery.subprocess.CalledProcessError):
                self.create(False)(['process','launch','bundle'],'launch')
        self.restart.assert_not_called();self.assertEqual(self.calls,1)

if __name__ == '__main__':
    unittest.main()
