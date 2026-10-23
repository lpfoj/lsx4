"""Meaningful negative routes: no capacity reuse across build/profile/backend changes."""
import json
import tempfile
import unittest
from pathlib import Path
from library_jit_pool_policy import select_prepared_pool


class PolicyContract(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.path = Path(self.directory.name) / 'policy.json'
        self.policy = {'schemaVersion': 1, 'binarySha256': 'runtime-a', 'titles': {
            'GAME': {'poolMiB': 256, 'backend': 'fex', 'profileSha256': 'profile-a',
                     'runId': '1234-abcd'}}}
        self.path.write_text(json.dumps(self.policy))

    def select(self, **kwargs):
        values = dict(path=self.path, title='GAME', backend='fex', profile_sha='profile-a',
                      binary_sha='runtime-a', default=512)
        values.update(kwargs)
        return select_prepared_pool(**values)

    def test_verified_capacity(self):
        pool, receipt = self.select()
        self.assertEqual((pool, receipt['source']), (256, 'observed-gameplay'))

    def test_changed_runtime_profile_backend_and_unknown_title(self):
        for values in (dict(binary_sha='runtime-b'), dict(profile_sha='profile-b'),
                       dict(backend='current'), dict(title='UNKNOWN')):
            with self.subTest(values=values):
                self.assertEqual(self.select(**values)[0], 512)

    def test_explicit_override_and_absent_policy(self):
        self.assertEqual(self.select(explicit=512)[0], 512)
        self.assertEqual(self.select(explicit=128)[0], 128)
        self.assertEqual(self.select(path=None)[0], 512)

    def test_corrupt_receipts_never_shrink_pool(self):
        for value in (64, 255, 1024, True, '256'):
            self.policy['titles']['GAME']['poolMiB'] = value
            self.path.write_text(json.dumps(self.policy))
            with self.subTest(value=value), self.assertRaises(ValueError):
                self.select()

    def test_no_receipt_run_id(self):
        self.policy['titles']['GAME']['runId'] = ''
        self.path.write_text(json.dumps(self.policy))
        with self.assertRaises(ValueError):
            self.select()


if __name__ == '__main__':
    unittest.main()
