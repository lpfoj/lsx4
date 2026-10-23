"""Verified Android CPU profile handoff shared by library and physical tests."""
import hashlib, json, re
from pathlib import Path
NAME='android-cpu-tuning-v1.ini'
SOURCE=Path(__file__).resolve().parents[2]/'android-app/app/src/main/java/app/lsx4/android/CpuTuningSettings.java'
def sha(path): return hashlib.sha256(path.read_bytes()).hexdigest()
def select_profile(directory,title):
    if not re.fullmatch(r'(?:CUSA|PPSA)[0-9]{5}',title):raise ValueError('Invalid profile title')
    manifest=json.loads((directory/'manifest.json').read_text())
    if manifest.get('version')!=1 or manifest.get('resolverSha256')!=sha(SOURCE):
        raise ValueError('Android CPU profiles are stale; regenerate from current source')
    profile=directory/(title+'.ini')
    if sha(profile)!=manifest['profiles'].get(title):raise ValueError('Android CPU profile checksum mismatch')
    return profile
def stage_profile(source,output,title):
    folder=output/'cpu-config';folder.mkdir(parents=True,exist_ok=True)
    data=source.read_bytes() if source else b'# Native Balanced defaults for this launch\n'
    if source and ('# title_id='+title) not in data.decode().splitlines():
        raise ValueError('CPU profile belongs to another title')
    target=folder/NAME;target.write_bytes(data)
    (output/'cpu-profile.json').write_text(json.dumps({'source':str(source) if source else 'native-default',
        'title':title,'sha256':sha(target)},indent=2)+'\n')
    return target

def resolve_backend(requested, profile):
    """Select before pool allocation; never rewrite the Android CPU profile.

    `auto` prefers FEX but routes its unsupported execution/memory policies to
    Current, as Android's native selection does. Explicit FEX remains strict:
    a rejected activation must be visible, not disguised by a retry/fallback.
    The native parser still validates and resolves the complete profile.
    """
    if requested not in ('current', 'fex', 'auto'):
        raise ValueError('Unsupported Apple CPU backend')
    decision = {'requested': requested, 'effective': requested, 'reason': 'explicit',
                'profileSha256': sha(profile)}
    if requested != 'auto':
        return decision
    fields = {}
    limits = {'execution_mode': 3, 'fast_memory': 2, 'memory_manager': 3,
              'ignore_bad_accesses': 1}
    for line in profile.read_text().splitlines():
        line = line.strip()
        if not line or line.startswith('#'):
            continue
        key, separator, value = line.partition('=')
        key = key.strip()
        if key not in limits:
            continue
        if key in fields or not separator or not re.fullmatch(r'[0-9]+', value.strip()):
            raise ValueError('Invalid backend policy field: ' + key)
        fields[key] = int(value)
        if not 0 <= fields[key] <= limits[key]:
            raise ValueError('Invalid backend policy field: ' + key)
    if fields.keys() != limits.keys():
        raise ValueError('Automatic backend selection requires a resolved Android CPU profile')
    reasons = []
    if fields['execution_mode'] != 0: reasons.append('execution_mode')
    if fields['fast_memory'] == 0: reasons.append('fast_memory')
    if fields['memory_manager'] in (0, 2): reasons.append('memory_manager')
    if fields['ignore_bad_accesses'] != 0: reasons.append('ignore_bad_accesses')
    decision.update(effective='current' if reasons else 'fex',
                    reason='profile-requires-current' if reasons else 'profile-allows-fex',
                    currentPolicyFields=reasons, profilePolicy=fields)
    return decision
