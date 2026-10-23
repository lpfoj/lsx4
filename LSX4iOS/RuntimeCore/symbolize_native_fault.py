"""Offline native fault symbols, bound to the archived binary and launch UUID."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

from device_run_evidence import current_run_stderr, inspect_stderr


def load_evidence(folder):
    identity = json.loads((folder / 'probe-launch.json').read_text())
    build = json.loads((folder / 'build.json').read_text())
    # Use the run-local immutable copy, never a later app at sourceApp.
    binary = folder / 'LSX4RuntimeProbe'
    if hashlib.sha256(binary.read_bytes()).hexdigest() != build['binarySha256']:
        raise ValueError('Archived binary does not match build receipt')
    contents = (folder / 'stderr.log').read_text(errors='replace')
    current = current_run_stderr(contents, identity['runId'])
    if current is None:
        raise ValueError('Stderr launch UUID does not match requested run')
    signals = re.findall(r'^\[APPLE_UNHANDLED_SIGNAL\][^\r\n]*', current, re.MULTILINE)
    if not signals:
        raise ValueError('No current-run native fatal signal')
    return binary, identity, build, inspect_stderr(contents, identity['runId']), signals


def anchor_runtime_address(lldb):
    # The stopped frame, not the pre-launch breakpoint address, contains ASLR.
    anchors = set(re.findall(
        r'frame #0: (0x[0-9a-fA-F]+) LSX4RuntimeProbe`LSX4RuntimeProbePreparedPoolReady(?:\s|$)',
        lldb))
    if len(anchors) != 1:
        raise ValueError('Missing or ambiguous runtime load-address anchor')
    return int(anchors.pop(), 16)


def run_tool(*args):
    return subprocess.run(args, check=True, capture_output=True, text=True,
                          timeout=60).stdout


def symbolize(folder):
    binary, identity, build, diagnostics, signals = load_evidence(folder)
    lldb = (folder / 'lldb.log').read_text()
    launched_pid = json.loads((folder / 'launch.json').read_text())['result']['process']['processIdentifier']
    attached_pids = set(re.findall(r'device process attach -p ([0-9]+)\b', lldb))
    if attached_pids != {str(launched_pid)}:
        raise ValueError('LLDB attachment does not match launched process')
    symbols = run_tool('xcrun', 'nm', '-arch', 'arm64', '-n', str(binary))
    anchor = re.search(r'^([0-9a-fA-F]+) [tT] _LSX4RuntimeProbePreparedPoolReady$',
                       symbols, re.MULTILINE)
    if not anchor:
        raise ValueError('Archived binary has no prepared-pool anchor symbol')
    runtime = anchor_runtime_address(lldb)
    slide = runtime - int(anchor.group(1), 16)
    segments = run_tool('xcrun', 'otool', '-l', str(binary))
    text = re.search(r'segname __TEXT\s+vmaddr (0x[0-9a-fA-F]+)\s+vmsize (0x[0-9a-fA-F]+)', segments)
    if not text:
        raise ValueError('Missing Mach-O __TEXT segment')
    image_base = int(text.group(1), 16) + slide
    text_end = image_base + int(text.group(2), 16)
    if slide < 0 or slide % 4096 or not image_base <= runtime < text_end:
        raise ValueError('Invalid runtime image slide')
    records = []
    for signal in signals:
        frames = []
        for field in ('pc', 'lr'):
            match = re.search(r'\b' + field + r'=(0x[0-9a-fA-F]+)\b', signal)
            if match:
                frames.append((field, int(match.group(1), 16)))
        native = re.search(r'\bnative_frames=\[([^]]*)\]', signal)
        if native:
            frames.extend(('frame' + str(i), int(value, 16)) for i, value in
                          enumerate(re.findall(r'0x[0-9a-fA-F]+', native.group(1))))
        entries = []
        for label, raw in frames:
            address = raw & 0x0000FFFFFFFFFFFF
            inside = image_base <= address < text_end
            symbol = run_tool('xcrun', 'atos', '-arch', 'arm64', '-o', str(binary),
                              '-l', hex(image_base), hex(address)).strip() if inside else None
            entries.append({'label': label, 'rawAddress': hex(raw), 'address': hex(address),
                            'insideArchivedText': inside, 'symbol': symbol})
        records.append({'diagnostic': signal, 'frames': entries})
    return {'runId': identity['runId'], 'binarySha256': build['binarySha256'],
            'archivedBinary': str(binary.resolve()), 'runtimeImageBase': hex(image_base),
            'slide': hex(slide), 'stderrEvidence': diagnostics, 'signals': records,
            'note': 'Chronological signals; later termination traps do not replace the first fault. '
                    'Symbols outside this archived image are deliberately unresolved.'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('run_folder', type=Path)
    args = parser.parse_args()
    result = symbolize(args.run_folder)
    path = args.run_folder / 'native-fault-symbols.json'
    path.write_text(json.dumps(result, indent=2) + '\n')
    print(path)
    for index, signal in enumerate(result['signals']):
        print('signal', index)
        for frame in signal['frames']:
            print(frame['label'], frame['address'], frame['symbol'] or '(outside archived image)')


if __name__ == '__main__':
    main()
