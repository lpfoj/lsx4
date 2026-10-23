"""One build-bound Android mapping for ordinary library launches.

This profile deliberately has no title entries or diagnostic overrides. Its
receipt identifies the same signed image that was tested by device_probe.
"""
import hashlib
import json
import plistlib


ANDROID_MAPPED_ENVIRONMENT = {
    'LSX4_APPLE_MANAGED_LAUNCH': 'options',
    'LSX4_APPLE_MONO_SELFCHECK_OBJECTS': '1',
    'LSX4_APPLE_MANAGED_READBACK_MODE': '1',
    'LSX4_APPLE_RENDER_LAUNCH': 'on',
    'LSX4_APPLE_VULKAN_POLICY': 'on',
    'LSX4_APPLE_RENDER_DIAGNOSTICS': 'quiet',
    'LSX4_APPLE_PHYSICAL_DRS_POLICY': 'mapped',
}
REQUIRED_LAYERS = {
    'android-managed-launch', 'apple-mono-selfcheck-objects',
    'apple-managed-readback-mode', 'apple-android-presentation',
    'apple-vulkan-driver-policy', 'apple-render-diagnostics',
    'apple-metal-fixed-surface', 'apple-physical-drs-policy',
}


def load_launch_policy(path, app, backend, cpu_profiles, verification=None):
    if path is None:
        return None
    if backend != 'auto' or cpu_profiles is None:
        raise ValueError('Android mapping requires --cpu-backend auto and --cpu-profiles')
    if verification is not None:
        raise ValueError('Ordinary Android mapping cannot be combined with per-title diagnostics')
    policy = json.loads(path.read_text())
    if (not isinstance(policy, dict) or set(policy) != {
            'schemaVersion', 'profile', 'bundleIdentifier', 'bundleVersion', 'binarySha256'}
            or type(policy['schemaVersion']) is not int or policy['schemaVersion'] != 1
            or policy['profile'] != 'android-mapped-v1'):
        raise ValueError('Invalid ordinary library launch policy')
    info = plistlib.loads((app / 'Info.plist').read_bytes())
    binary_hash = hashlib.sha256((app / 'LSX4RuntimeProbe').read_bytes()).hexdigest()
    if (policy['bundleIdentifier'] != 'com.llayerlinux.LSX4RuntimeProbe'
            or policy['bundleIdentifier'] != info.get('CFBundleIdentifier')
            or policy['bundleVersion'] != info.get('CFBundleVersion')
            or policy['binarySha256'] != binary_hash):
        raise ValueError('Library launch policy does not match the signed local app')
    evidence = json.loads((app.parent / 'build-evidence.json').read_text())
    layers = {layer['name'] for layer in evidence.get('experiments', [])}
    if not REQUIRED_LAYERS <= layers:
        raise ValueError('Local app lacks required Android mapping layers')
    return dict(policy, environment=dict(ANDROID_MAPPED_ENVIRONMENT))
