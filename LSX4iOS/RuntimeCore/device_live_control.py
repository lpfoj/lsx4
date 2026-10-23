#!/usr/bin/env python3
"""Send one bounded pad state to an explicitly enabled, current iPhone test run.

Does not install, launch, attach a debugger, or restart an app. Live input and
captures are diagnostic work and must not be used for FPS acceptance.
"""
import argparse
import fcntl
import json
import math
from pathlib import Path
import subprocess
import time
import uuid

BUNDLE = 'com.llayerlinux.LSX4RuntimeProbe'
MASK = 0x11ffff


def packet(run, sequence, buttons, axes, hold, capture):
    if str(uuid.UUID(run)) != run or not 1 <= sequence < 1000000000:
        raise ValueError('Invalid run identity or sequence')
    if buttons < 0 or buttons & ~MASK or len(axes) != 6 or any(not 0 <= x <= 255 for x in axes):
        raise ValueError('Invalid controller state')
    if not math.isfinite(hold) or not 0 <= hold <= 10:
        raise ValueError('Hold duration must be finite and at most 10 seconds')
    if hold == 0 and (buttons or list(axes) != [128,128,128,128,0,0]):
        raise ValueError('Non-neutral state needs a positive hold duration')
    return f'{run} {sequence} {hold:.6f} {buttons:x} ' + ' '.join(map(str, axes)) + f' {int(capture)}\n'


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--run-output', type=Path, required=True)
    p.add_argument('--buttons', type=lambda x: int(x,16), default=0, help='Orbis mask in hex; Cross=4000, right=20, left=80')
    p.add_argument('--axes', type=int, nargs=6, default=[128,128,128,128,0,0])
    p.add_argument('--hold', type=float, default=0.3)
    p.add_argument('--capture', action='store_true', help='Request a frame after releasing the command')
    args = p.parse_args()
    metadata = json.loads((args.run_output / 'live-control.json').read_text())
    run = metadata['runId']; remote = 'Documents/live-control-' + run
    if metadata['directory'] != remote:
        raise ValueError('Live control directory does not match its launch identity')
    # Per-run serialization prevents two host invocations from reusing a number.
    commands = args.run_output / 'live-commands'; commands.mkdir(exist_ok=True)
    with (commands / 'owner.lease').open('a') as lease:
        fcntl.flock(lease, fcntl.LOCK_EX)
        counter = commands / 'sequence.txt'
        sequence = (int(counter.read_text()) if counter.exists() else 0) + 1
        wire = packet(run, sequence, args.buttons, args.axes, args.hold, args.capture)
        output = commands / f'{sequence:03d}'; output.mkdir()
        source = output / 'command.txt'; source.write_text(wire)
        counter.write_text(str(sequence)) # Never reuse an uncertain sent command.
        def device(parts, name):
            with (output / (name + '.log')).open('w') as log:
                subprocess.run(['xcrun','devicectl','device',*parts[:2], '--device', metadata['device'],
                    *parts[2:], '--domain-type','appDataContainer','--domain-identifier',BUNDLE,
                    '--timeout','10','--json-output',str(output / (name + '.json'))],
                    stdout=log,stderr=subprocess.STDOUT,check=True,timeout=20)
        # A stopped old run cannot be revived by sending a new command.
        before = output / 'before.json'
        device(['copy','from','--source',remote+'/state.json','--destination',str(before)],'read-before')
        state = json.loads(before.read_text())
        if state['phase'] == 'stopped' or state['sequence'] >= sequence:
            raise RuntimeError('Run stopped or another client advanced its input sequence')
        sent_unix = time.time()
        device(['copy','to','--source',str(source),'--destination',remote+'/command.txt'],'send')
        deadline = time.monotonic() + args.hold + 10
        attempt = 0
        while True:
            attempt += 1; after = output / f'state-{attempt}.json'
            device(['copy','from','--source',remote+'/state.json','--destination',str(after)],f'read-{attempt}')
            state = json.loads(after.read_text())
            if state['sequence'] == sequence and state['phase'] in ('released','stopped'):
                break
            if state['sequence'] > sequence or time.monotonic() >= deadline:
                raise RuntimeError('Command was superseded or acknowledgement deadline expired; do not blindly resend')
            time.sleep(0.25)
        receipt = {'runId':run,'sequence':sequence,'state':state,'fpsAcceptanceEligible':False,
                   'sentUnix':sent_unix,'acknowledgedUnix':time.time(),
                   'command':{'buttons':args.buttons,'axes':args.axes,'holdSeconds':args.hold,'capture':args.capture}}
        if args.capture:
            known_path = commands / 'known-frames.json'
            known = set(json.loads(known_path.read_text())) if known_path.exists() else set()
            fresh = []
            for retry in range(3):
                captured = output / f'capture-{retry}'
                device(['copy','from','--source',remote,'--destination',str(captured)],f'capture-{retry}')
                files = sorted(captured.glob('frame-*.png'))
                fresh = [str(path.resolve()) for path in files if path.name not in known]
                if fresh:
                    known.update(path.name for path in files);break
                time.sleep(0.5)
            receipt['newFrames'] = fresh
            known_path.write_text(json.dumps(sorted(known))+'\n')
            if not fresh:
                receipt['captureError'] = 'No newly published frame; retained images are not current evidence'
        (output / 'receipt.json').write_text(json.dumps(receipt,indent=2)+'\n')
        print(json.dumps(receipt))


if __name__ == '__main__':
    main()
