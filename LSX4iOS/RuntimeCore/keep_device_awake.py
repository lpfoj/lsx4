#!/usr/bin/env python3
"""Check the iPhone awake state without starting the heavy app by default.

The old watchdog activated the probe every eight seconds for hours. That made
the large iOS process reopen after a test and kept stealing the foreground.
The default is now a single passive lock-state check. Use ``--activate-once``
for one explicit foreground activation, or combine ``--watch --activate`` for
an intentional bounded watchdog session. It cannot unlock a phone.
"""
import argparse
import fcntl
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time

PROJECT=Path(__file__).resolve().parent.parent
BUNDLE='com.llayerlinux.LSX4RuntimeProbe'

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--device',required=True)
    parser.add_argument('--hours',type=float,default=12)
    parser.add_argument('--watch',action='store_true',
                        help='repeat checks until --hours expires')
    parser.add_argument('--activate',action='store_true',
                        help='allow foreground activation while watching')
    parser.add_argument('--activate-once',action='store_true',
                        help='activate the probe once, then exit')
    parser.add_argument('--once',action='store_true',help=argparse.SUPPRESS)
    args=parser.parse_args()
    if args.once:
        args.activate_once=True
    if args.activate_once and args.watch:
        parser.error('--activate-once cannot be combined with --watch')
    if args.activate and not args.watch:
        parser.error('--activate requires --watch; use --activate-once for one launch')
    # The Wi-Fi watchdog also requires an awake Mac between game runs.
    # This assertion ends with the watchdog rather than changing power settings.
    subprocess.Popen(['/usr/bin/caffeinate','-dimsu','-w',str(os.getpid())],
                     stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
    deadline=time.monotonic()+args.hours*3600
    lease_path=PROJECT/'.build/device-probe.lease'
    lease_path.parent.mkdir(parents=True,exist_ok=True)
    def device(arguments):
        with tempfile.TemporaryDirectory(prefix='lsx4-awake-') as folder:
            report=Path(folder)/'result.json'
            command=['xcrun','devicectl','device',*arguments[:2],'--device',args.device,
                     *arguments[2:],'--timeout','8','--json-output',str(report)]
            result=subprocess.run(command,stdout=subprocess.DEVNULL,stderr=subprocess.PIPE,
                                  text=True,timeout=9)
            if result.returncode:
                raise RuntimeError(result.stderr.strip() or 'device command failed')
            return json.loads(report.read_text())['result']
    last_error=None
    with lease_path.open('a') as lease:
        while True:
            try:
                fcntl.flock(lease,fcntl.LOCK_EX|fcntl.LOCK_NB)
            except BlockingIOError:
                if not args.watch: return
                time.sleep(8)
                continue
            try:
                state=device(['info','lockState'])
                if state.get('passcodeRequired',True):
                    raise RuntimeError('Locked: passcode required')
                if args.activate or args.activate_once:
                    # A suspended/background process cannot suppress auto-lock.
                    # Foreground the existing UI without terminating it. This
                    # is opt-in so a passive check never reopens the heavy app.
                    try:
                        device(['process','launch','--activate','--environment-variables',
                                '{"LSX4_CORE_KEEP_AWAKE":"1"}',BUNDLE])
                    except Exception:
                        device(['process','launch','--activate','com.llayerlinux.LSX4iOS'])
                    print(time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime()),
                          'Activated awake UI once' if args.activate_once
                          else 'Activated awake UI',flush=True)
                elif last_error is not None or not args.watch:
                    print(time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime()),
                          'Checked device lock state; no app activation',flush=True)
                last_error=None
            except Exception as error:
                message='Device locked; recovery awaits a manual unlock' if 'Locked' in str(error) else str(error)
                if message!=last_error:
                    print(time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime()),message,flush=True)
                    last_error=message
            finally:
                fcntl.flock(lease,fcntl.LOCK_UN)
            if args.activate_once or not args.watch: return
            if time.monotonic() >= deadline: return
            time.sleep(8)

if __name__=='__main__': main()
