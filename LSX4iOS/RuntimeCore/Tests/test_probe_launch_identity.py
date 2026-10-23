"""Exercise launch/recovery metadata with a fake device command boundary."""
import contextlib
import importlib.util
import io
import json
import plistlib
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch


CORE = Path(__file__).resolve().parents[1]
with patch.object(sys, 'path', [str(CORE), *sys.path]):
    spec = importlib.util.spec_from_file_location('probe_identity_under_test', CORE / 'device_probe.py')
    probe = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(probe)
    from device_run_evidence import confirmed_awake_recovery


class ProbeLaunchIdentityTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.app = self.root / 'probe.app'
        self.app.mkdir()
        (self.app / 'LSX4RuntimeProbe').write_bytes(b'local test image')
        (self.app / 'Info.plist').write_bytes(plistlib.dumps({
            'CFBundleIdentifier': probe.BUNDLE, 'CFBundleVersion': 'test.1'}))
        self.output = self.root / 'evidence'
        self.launch_ids = []
        self.launch_environments = []
        self.fail_launches = False
        self.fail_copy = False

    def fake_run(self, command, **kwargs):
        if command[:2] == ['cp', '-c']:
            if self.fail_copy:
                raise subprocess.CalledProcessError(1, command)
            shutil.copyfile(command[2], command[3])
            return subprocess.CompletedProcess(command, 0)
        self.assertEqual(command[:3], ['xcrun', 'devicectl', 'device'])
        if command[3:5] == ['process', 'launch']:
            if '--environment-variables' in command:
                environment = json.loads(command[command.index('--environment-variables') + 1])
                if 'LSX4_PROBE_RUN_ID' in environment:
                    self.launch_environments.append(environment)
                    run_id = environment['LSX4_PROBE_RUN_ID']
                    # Must already exist when the command crosses the device boundary.
                    launch = json.loads((self.output / 'probe-launch.json').read_text())
                    self.assertEqual(launch['runId'], run_id)
                    self.launch_ids.append(run_id)
                else:
                    self.assertEqual(environment, {'LSX4_CORE_KEEP_AWAKE': '1'})
            if self.fail_launches:
                raise subprocess.CalledProcessError(1, command)
        if command[3:5] == ['copy', 'from']:
            destination = Path(command[command.index('--destination') + 1])
            if not destination.is_dir():
                destination.write_text(json.dumps({'passed': True, 'timestamp': 1000}))
        result = Path(command[command.index('--json-output') + 1])
        payload = {'process': {'processIdentifier': 123}}
        if command[3:5] == ['info', 'apps']:
            payload = {'apps': [{'bundleIdentifier': probe.BUNDLE, 'bundleVersion': 'test.1'}]}
        result.write_text(json.dumps({'result': payload}))
        return subprocess.CompletedProcess(command, 0)

    def run_probe(self, *extra_args):
        argv = ['device_probe.py', '--device', 'fake', '--app', str(self.app),
                '--output', str(self.output), '--interpreter', '--skip-install', *extra_args]
        actual_open = Path.open
        leases = []
        def tracked_open(path, *args, **kwargs):
            handle = actual_open(path, *args, **kwargs)
            if path.name == 'device-probe.lease':
                leases.append(handle)
            return handle
        with patch.object(sys, 'argv', argv), patch.object(probe, 'PROJECT', self.root), \
                patch.object(Path, 'open', tracked_open), \
                patch.object(probe.subprocess, 'Popen'), \
                patch.object(probe.subprocess, 'run', side_effect=self.fake_run), \
                patch.object(probe.time, 'time', return_value=500), \
                contextlib.redirect_stdout(io.StringIO()):
            try:
                probe.main()
            finally:
                for lease in leases:
                    lease.close()

    def test_each_launch_gets_a_new_uuid_before_device_command(self):
        self.run_probe()
        self.run_probe()
        self.assertEqual(len(set(self.launch_ids)), 2)
        for run_id in self.launch_ids:
            self.assertEqual(str(probe.uuid.UUID(run_id)), run_id)
        recovery = json.loads((self.output / 'probe-recovery.json').read_text())
        self.assertEqual(recovery, {'runId': self.launch_ids[-1], 'attempted': False,
                                    'awakeRecovered': True})

    def test_unavailable_live_input_and_managed_modes_fail_before_launch(self):
        for option in [('--live-input',), ('--managed-launch', 'full'), ('--mono-selfcheck-objects', 'on'),
                       ('--managed-disable-options', '7'), ('--managed-readback-mode', 'on'),
                       ('--render-launch','on')]:
            with self.subTest(option=option), contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit):
                    self.run_probe(*option, '--game', 'Games/CUSA12345/eboot.bin', '--game-stage', 'run')
            self.assertFalse(self.launch_ids)

    def test_invalid_managed_option_controls_rejected_before_device_boundary(self):
        (self.root/'build-evidence.json').write_text(json.dumps({'experiments':[
            {'name':'android-managed-launch'}, {'name':'apple-managed-option-control'}]}))
        for values in [('0',),('22',),('7','7')]:
            with self.subTest(values=values), contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit):
                    self.run_probe('--managed-launch','options','--managed-disable-options',*values,
                                   '--game','Games/CUSA12345/eboot.bin','--game-stage','run')
            self.assertFalse(self.launch_ids)

    def test_requested_capture_excludes_fps_acceptance(self):
        self.run_probe('--capture-window', '5 10 2')
        diagnostics=json.loads((self.output/'diagnostics.json').read_text())
        self.assertIn('capture_window',diagnostics['flags'])
        self.assertFalse(diagnostics['fpsAcceptanceEligible'])

    def test_stale_candidate_is_not_published_as_current_report(self):
        original_run = self.fake_run
        copied = []
        def copy_stale_then_fresh(command, **kwargs):
            result = original_run(command, **kwargs)
            if command[3:5] == ['copy', 'from']:
                destination = Path(command[command.index('--destination') + 1])
                if destination.name == 'report-candidate.json':
                    self.assertFalse((self.output/'report.json').exists())
                    copied.append(destination)
                    if len(copied) == 1:
                        destination.write_text(json.dumps({'passed': True, 'timestamp': 499}))
            return result
        with patch.object(self, 'fake_run', side_effect=copy_stale_then_fresh), \
                patch.object(probe.time, 'sleep'):
            self.run_probe()
        self.assertEqual(len(copied), 2)
        self.assertEqual(json.loads((self.output/'report.json').read_text())['timestamp'], 1000)
        self.assertFalse((self.output/'report-candidate.json').exists())
        evidence = json.loads((self.output/'report-evidence.json').read_text())
        self.assertEqual(evidence, {'runId': self.launch_ids[-1], 'launchStartedUnix': 500,
                                    'reportTimestamp': 1000, 'freshForLaunch': True})

    def test_jit_perf_map_is_opt_in_and_excluded_from_fps_acceptance(self):
        self.run_probe()
        self.assertNotIn('LSX4_PROBE_JIT_PERF_MAP', self.launch_environments[-1])
        self.run_probe('--jit-perf-map')
        self.assertEqual(self.launch_environments[-1]['LSX4_PROBE_JIT_PERF_MAP'], '1')
        diagnostic = json.loads((self.output / 'diagnostics.json').read_text())
        self.assertIn('jit_perf_map', diagnostic['flags'])
        self.assertFalse(diagnostic['fpsAcceptanceEligible'])
        self.assertTrue((self.output / 'jit-perf.map').exists())

    def test_jit_register_diagnostics_are_opt_in_and_never_fps_acceptance(self):
        self.run_probe()
        for option, variable, name in (
            ('--jit-memory-edges', 'EXECUTOR_DIAG_JIT_MEMORY_EDGES', 'jit_memory_edges'),
            ('--jit-basic-edges', 'EXECUTOR_DIAG_JIT_BASIC_EDGES', 'jit_basic_edges'),
            ('--jit-local-xmm', 'EXECUTOR_DIAG_JIT_LOCAL_XMM', 'jit_local_xmm'),
        ):
            self.assertNotIn(variable, self.launch_environments[0])
            self.run_probe(option)
            self.assertEqual(self.launch_environments[-1][variable], '1')
            diagnostic = json.loads((self.output / 'diagnostics.json').read_text())
            self.assertIn(name, diagnostic['flags'])
            self.assertFalse(diagnostic['fpsAcceptanceEligible'])

    def test_unlinked_fixture_is_opt_in_and_cannot_change_game_policy(self):
        self.run_probe()
        self.assertNotIn('LSX4_TEST_UNLINKED_NATIVE', self.launch_environments[-1])
        self.run_probe('--unlinked-fixture')
        self.assertEqual(self.launch_environments[-1]['LSX4_TEST_UNLINKED_NATIVE'], '1')
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            self.run_probe('--unlinked-fixture', '--game', 'CUSA00000/eboot.bin')
        self.assertEqual(len(self.launch_ids), 2)

    def test_sampled_texture_dump_is_bounded_opt_in_diagnostic(self):
        self.run_probe('--gpu-content-probe', '--gpu-content-extent', '256', '284')
        env = self.launch_environments[-1]
        self.assertEqual(env['EXECUTOR_GPU_CONTENT_SAMPLE_WIDTH'], '256')
        self.assertEqual(env['EXECUTOR_GPU_CONTENT_SAMPLE_HEIGHT'], '284')
        self.assertEqual(env['LSX4_PROBE_GPU_TEXTURE_DUMP'], '1')
        self.assertTrue((self.output / 'sampled-textures').exists())
        self.assertFalse(json.loads((self.output / 'diagnostics.json').read_text())['fpsAcceptanceEligible'])
        for options in [('--gpu-content-extent','256','284'),
                        ('--gpu-content-probe','--gpu-content-extent','0','284'),
                        ('--gpu-content-probe','--gpu-content-extent','256','16385')]:
            with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                self.run_probe(*options)
        self.assertEqual(len(self.launch_ids), 1)

    def test_shader_capture_is_opt_in_and_excluded_from_fps_acceptance(self):
        self.run_probe()
        self.assertNotIn('LSX4_PROBE_SHADER_DUMP', self.launch_environments[-1])
        self.run_probe('--shader-dumps', '0x1234', '0x5678')
        env = self.launch_environments[-1]
        self.assertEqual(env['LSX4_PROBE_SHADER_DUMP'], '1')
        self.assertEqual(env['EXECUTOR_SHADER_DUMP_HASHES'], '0x1234 0x5678')
        self.assertTrue((self.output / 'shader-dumps').is_dir())
        diagnostic = json.loads((self.output / 'diagnostics.json').read_text())
        self.assertIn('shader_dumps', diagnostic['flags'])
        self.assertFalse(diagnostic['fpsAcceptanceEligible'])

    def test_draw_attachment_dump_does_not_require_texture_extent(self):
        self.run_probe()
        self.assertNotIn('LSX4_PROBE_GPU_TEXTURE_DUMP', self.launch_environments[-1])
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            self.run_probe('--gpu-content-dumps')
        self.assertEqual(len(self.launch_ids), 1)
        self.run_probe('--gpu-content-probe', '--gpu-content-dumps')
        env = self.launch_environments[-1]
        self.assertEqual(env['LSX4_PROBE_GPU_TEXTURE_DUMP'], '1')
        self.assertNotIn('EXECUTOR_GPU_CONTENT_SAMPLE_WIDTH', env)
        self.assertTrue((self.output / 'sampled-textures').is_dir())
        self.assertFalse(json.loads((self.output / 'diagnostics.json').read_text())['fpsAcceptanceEligible'])

    def test_heap_fixture_selects_fresh_process_mode(self):
        self.run_probe('--heap-fixture')
        self.assertEqual(self.launch_environments[0]['LSX4_CORE_AEROLIB_HEAP_FIXTURE'], '1')
        self.assertNotIn('LSX4_CORE_GAME', self.launch_environments[0])

    def test_heap_fixture_rejects_game_before_device_setup(self):
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            self.run_probe('--heap-fixture', '--game', 'CUSA00000/eboot.bin')
        self.assertFalse(self.launch_ids)
        self.assertFalse(self.output.exists())

    def test_failed_awake_recovery_is_recorded_for_current_run(self):
        self.fail_launches = True
        with self.assertRaises(subprocess.CalledProcessError):
            self.run_probe()
        recovery = json.loads((self.output / 'probe-recovery.json').read_text())
        self.assertEqual(recovery, {'runId': self.launch_ids[-1], 'attempted': True,
                                    'awakeRecovered': False})
        self.assertFalse(confirmed_awake_recovery(recovery, self.launch_ids[-1]))

    def test_early_setup_failure_cannot_reuse_previous_recovery(self):
        self.run_probe()
        old_run_id = self.launch_ids[-1]
        (self.output / 'probe-recovery.json').write_text(json.dumps({
            'runId': old_run_id, 'attempted': True, 'awakeRecovered': True}))
        self.fail_copy = True
        with self.assertRaises(subprocess.CalledProcessError):
            self.run_probe()
        current = json.loads((self.output / 'probe-launch.json').read_text())['runId']
        self.assertNotEqual(current, old_run_id)
        recovery = json.loads((self.output / 'probe-recovery.json').read_text())
        self.assertFalse(confirmed_awake_recovery(recovery, current))

    def test_awake_confirmation_requires_matching_identity_and_explicit_success(self):
        recovery = {'runId': 'current', 'attempted': True, 'awakeRecovered': True}
        self.assertTrue(confirmed_awake_recovery(recovery, 'current'))
        for change in ({'runId': 'old'}, {'attempted': False}, {'awakeRecovered': False},
                       {'awakeRecovered': 'true'}):
            self.assertFalse(confirmed_awake_recovery({**recovery, **change}, 'current'))
        self.assertFalse(confirmed_awake_recovery({}, None))


if __name__ == '__main__':
    unittest.main()
