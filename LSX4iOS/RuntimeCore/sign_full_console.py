#!/usr/bin/env python3
"""Sign the full iPhone runtime with profile-authorized address-space support."""
import argparse
import datetime
import json
from pathlib import Path
import plistlib
import shutil
import subprocess

EXTENDED_VA = 'com.apple.developer.kernel.extended-virtual-addressing'
INCREASED_MEMORY = 'com.apple.developer.kernel.increased-memory-limit'


def signing_entitlements(profile, bundle, now=None):
    now = now or datetime.datetime.now(datetime.timezone.utc)
    expiry = profile['ExpirationDate'].replace(tzinfo=datetime.timezone.utc)
    if expiry <= now:
        raise ValueError('Provisioning profile has expired')
    allowed = profile['Entitlements']
    app_id = allowed['application-identifier']
    # Require the exact runtime identifier; never reuse another app's profile.
    if app_id.split('.', 1)[1] != bundle:
        raise ValueError('Provisioning profile does not match the app bundle')
    if not allowed.get('get-task-allow'):
        raise ValueError('Development profile with debugger access is required')
    if allowed.get(EXTENDED_VA) is not True:
        raise ValueError('Profile does not authorize Extended Virtual Addressing; '
                         'enable the capability through Xcode and refresh the profile')
    if allowed.get(INCREASED_MEMORY) is not True:
        raise ValueError('Profile does not authorize Increased Memory Limit; '
                         'enable the capability through Xcode and refresh the profile')
    return {key: allowed[key] for key in (
        'application-identifier', 'com.apple.developer.team-identifier',
        'get-task-allow', EXTENDED_VA, INCREASED_MEMORY)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--app', required=True, type=Path)
    parser.add_argument('--profile', required=True, type=Path)
    parser.add_argument('--identity', required=True)
    parser.add_argument('--output', required=True, type=Path,
                        help='Local signing evidence directory (contains no private keys)')
    args = parser.parse_args()
    info = plistlib.loads((args.app / 'Info.plist').read_bytes())
    profile = plistlib.loads(subprocess.check_output(
        ['security', 'cms', '-D', '-i', str(args.profile)]))
    entitlements = signing_entitlements(profile, info['CFBundleIdentifier'])
    args.output.mkdir(parents=True, exist_ok=True)
    entitlement_file = args.output / 'signing.entitlements'
    entitlement_file.write_bytes(plistlib.dumps(entitlements))
    shutil.copyfile(args.profile, args.app / 'embedded.mobileprovision')
    subprocess.run(['codesign', '--force', '--sign', args.identity, '--entitlements',
                    str(entitlement_file), '--generate-entitlement-der', str(args.app)], check=True)
    subprocess.run(['codesign', '--verify', '--strict', str(args.app)], check=True)
    signed = plistlib.loads(subprocess.check_output(
        ['codesign', '-d', '--entitlements', ':-', str(args.app)], stderr=subprocess.DEVNULL))
    if any(signed.get(key) != value for key, value in entitlements.items()):
        raise RuntimeError('Signed entitlements differ from validated profile permissions')
    report = {'bundle': info['CFBundleIdentifier'], 'build': info['CFBundleVersion'],
              'extendedVirtualAddressing': True, 'increasedMemoryLimit': True,
              'signatureVerified': True,
              'profileExpires': profile['ExpirationDate'].isoformat()}
    (args.output / 'signing.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report))


if __name__ == '__main__':
    main()
