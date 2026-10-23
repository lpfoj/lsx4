"""Reject stale installed app versions before a physical-device observation."""


def verify_installed_build(result, local_info):
    bundle = local_info.get('CFBundleIdentifier')
    version = local_info.get('CFBundleVersion')
    if not isinstance(bundle, str) or not bundle or not isinstance(version, str) or not version:
        raise RuntimeError('Local app has no bundle identifier/build version')
    matches = [app for app in result.get('apps', []) if app.get('bundleIdentifier') == bundle]
    if len(matches) != 1 or matches[0].get('bundleVersion') != version:
        actual = [app.get('bundleVersion') for app in matches]
        raise RuntimeError(f'Installed app differs from local symbols: expected {bundle} build {version}, found {actual}; no guest launch permitted')
    return {'bundleIdentifier': bundle, 'bundleVersion': version,
            'installedURL': matches[0].get('url'), 'versionMatched': True,
            'binaryHashVerifiedOnDevice': False}
