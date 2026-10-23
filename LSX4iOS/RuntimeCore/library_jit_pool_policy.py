"""Select a previously observed iPhone JIT capacity for an exact runtime/profile.

This host-only policy does not change executable allocation or page preparation.
Unknown artifacts, backends and profiles keep the ordinary launcher's default.
Explicit capacity overrides retain their original priority.
"""
import json
import re


def select_prepared_pool(path, title, backend, profile_sha, binary_sha, default, explicit=None):
    selected = max(64, min(512, explicit if explicit is not None else default))
    receipt = {'poolMiB': selected, 'source': 'explicit' if explicit is not None else 'default'}
    if explicit is not None or path is None:
        return selected, receipt
    policy = json.loads(path.read_text())
    if policy.get('schemaVersion') != 1:
        raise ValueError('Unsupported JIT pool policy schema')
    if policy.get('binarySha256') != binary_sha:
        receipt['reason'] = 'runtime-identity-mismatch'
        return selected, receipt
    candidate = policy.get('titles', {}).get(title)
    if not candidate:
        receipt['reason'] = 'no-gameplay-capacity-receipt'
        return selected, receipt
    if candidate.get('backend') != backend or candidate.get('profileSha256') != profile_sha:
        receipt['reason'] = 'backend-or-profile-mismatch'
        return selected, receipt
    capacity = candidate.get('poolMiB')
    if (type(capacity) is not int or capacity not in (128, 256, 512)
            or not re.fullmatch(r'[A-Za-z0-9-]{1,80}', str(candidate.get('runId', '')))):
        raise ValueError('Invalid gameplay JIT capacity receipt')
    receipt.update(poolMiB=capacity, source='observed-gameplay',
                   runId=candidate['runId'], backend=backend, profileSha256=profile_sha)
    return capacity, receipt
