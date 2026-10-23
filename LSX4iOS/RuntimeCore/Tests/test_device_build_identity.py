import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from device_build_identity import verify_installed_build


class InstalledBuildIdentityTests(unittest.TestCase):
    def setUp(self):
        self.local = {'CFBundleIdentifier': 'com.llayerlinux.LSX4RuntimeProbe',
                      'CFBundleVersion': '128.9015', 'CFBundleShortVersionString': '1.0'}
        self.app = {'bundleIdentifier': self.local['CFBundleIdentifier'],
                    'bundleVersion': '128.9015', 'version': '1.0'}

    def test_exact_build_with_unrelated_app(self):
        result = verify_installed_build({'apps': [{'bundleIdentifier': 'other'}, self.app]}, self.local)
        self.assertTrue(result['versionMatched'])
        self.assertFalse(result['binaryHashVerifiedOnDevice'])

    def test_raced_diagnostic_install_same_marketing_version(self):
        self.app['bundleVersion'] = '128.9018'
        with self.assertRaisesRegex(RuntimeError, 'no guest launch permitted'):
            verify_installed_build({'apps': [self.app]}, self.local)

    def test_missing_or_ambiguous_device_record(self):
        for apps in ([], [self.app, self.app], [{'bundleIdentifier': self.app['bundleIdentifier']} ]):
            with self.subTest(apps=apps), self.assertRaises(RuntimeError):
                verify_installed_build({'apps': apps}, self.local)

    def test_missing_local_build_does_not_match_unknown_remote(self):
        self.local.pop('CFBundleVersion')
        with self.assertRaises(RuntimeError):
            verify_installed_build({'apps': []}, self.local)


if __name__ == '__main__':
    unittest.main()
