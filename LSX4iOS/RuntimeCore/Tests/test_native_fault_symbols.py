"""Reject stale identity and unrelocated addresses before offline symbolization."""
import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from symbolize_native_fault import anchor_runtime_address, load_evidence


class NativeFaultEvidenceTests(unittest.TestCase):
    def test_prelaunch_breakpoint_is_not_a_runtime_address(self):
        with self.assertRaises(ValueError):
            anchor_runtime_address('Breakpoint 2: where = LSX4RuntimeProbe`LSX4RuntimeProbePreparedPoolReady, address = 0x1000d3d94')
        self.assertEqual(anchor_runtime_address('    frame #0: 0x102e8fd94 LSX4RuntimeProbe`LSX4RuntimeProbePreparedPoolReady\n'), 0x102e8fd94)

    def test_conflicting_runtime_loads_are_rejected(self):
        with self.assertRaises(ValueError):
            anchor_runtime_address('frame #0: 0x102e8fd94 LSX4RuntimeProbe`LSX4RuntimeProbePreparedPoolReady\n'
                                   'frame #0: 0x103e8fd94 LSX4RuntimeProbe`LSX4RuntimeProbePreparedPoolReady\n')

    def test_identity_and_binary_are_checked_before_symbolization(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp)
            (path/'LSX4RuntimeProbe').write_bytes(b'immutable run binary')
            (path/'build.json').write_text(json.dumps({'binarySha256':hashlib.sha256(b'immutable run binary').hexdigest()}))
            (path/'probe-launch.json').write_text(json.dumps({'runId':'requested'}))
            fault='[APPLE_UNHANDLED_SIGNAL] signal=0xb fault=0x8'
            (path/'stderr.log').write_text('[APPLE_PROBE_RUN] id=other\n'+fault)
            with self.assertRaisesRegex(ValueError,'UUID'):
                load_evidence(path)
            (path/'stderr.log').write_text('[APPLE_PROBE_RUN] id=requested\n'+fault)
            self.assertEqual(load_evidence(path)[4], [fault])
            (path/'LSX4RuntimeProbe').write_bytes(b'later build')
            with self.assertRaisesRegex(ValueError,'binary'):
                load_evidence(path)


if __name__ == '__main__':
    unittest.main()
