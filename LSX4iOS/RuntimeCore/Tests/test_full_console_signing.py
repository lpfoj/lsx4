import copy
import datetime
import sys
from pathlib import Path
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from sign_full_console import EXTENDED_VA, INCREASED_MEMORY, signing_entitlements


class FullConsoleSigningTests(unittest.TestCase):
    def setUp(self):
        self.now = datetime.datetime(2026, 10, 2, tzinfo=datetime.timezone.utc)
        self.bundle = 'com.example.runtime'
        self.profile = {
            'ExpirationDate': datetime.datetime(2027, 10, 2),
            'Entitlements': {'application-identifier': 'TEAM.' + self.bundle,
                'com.apple.developer.team-identifier': 'TEAM',
                'get-task-allow': True, EXTENDED_VA: True, INCREASED_MEMORY: True}}

    def test_authorized_profile(self):
        before = copy.deepcopy(self.profile)
        entitlements = signing_entitlements(self.profile, self.bundle, self.now)
        self.assertTrue(entitlements[EXTENDED_VA])
        self.assertEqual(self.profile, before)

    def test_missing_capability_cannot_silently_regress(self):
        for key in (EXTENDED_VA, INCREASED_MEMORY):
            with self.subTest(key=key):
                profile = copy.deepcopy(self.profile)
                del profile['Entitlements'][key]
                with self.assertRaisesRegex(ValueError, 'does not authorize'):
                    signing_entitlements(profile, self.bundle, self.now)

    def test_wrong_bundle_expired_or_distribution_profile_rejected(self):
        for change in ('bundle', 'expired', 'debugger'):
            with self.subTest(change=change):
                profile = copy.deepcopy(self.profile)
                if change == 'bundle':
                    profile['Entitlements']['application-identifier'] = 'TEAM.com.example.other'
                elif change == 'expired':
                    profile['ExpirationDate'] = datetime.datetime(2026, 10, 1)
                else:
                    profile['Entitlements']['get-task-allow'] = False
                with self.assertRaises(ValueError):
                    signing_entitlements(profile, self.bundle, self.now)


if __name__ == '__main__':
    unittest.main()
