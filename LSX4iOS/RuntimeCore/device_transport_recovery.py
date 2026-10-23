"""Bounded recovery of the caller's CoreDevice file service between observations.

Caller must hold device-probe.lease and have no attached debugger/live observation.
This never launches an app, alters pairing, or touches simulator/ADB services.
"""
import os
import signal
import subprocess
import time

SERVICE = '/Library/Developer/PrivateFrameworks/CoreDevice.framework/Versions/A/XPCServices/CoreDeviceService.xpc/Contents/MacOS/CoreDeviceService'


def restart_user_coredevice():
    processes = subprocess.check_output(['ps', '-axo', 'pid=,uid=,comm='], text=True, timeout=5)
    restarted = []
    for line in processes.splitlines():
        fields = line.split(None, 2)
        if len(fields) != 3 or not fields[0].isdigit() or not fields[1].isdigit():
            continue
        if int(fields[1]) != os.getuid() or fields[2] != SERVICE:
            continue
        pid = int(fields[0])
        if pid <= 1:
            continue
        try:
            os.kill(pid, signal.SIGTERM)
            restarted.append(pid)
        except ProcessLookupError:
            pass
    if restarted:
        time.sleep(0.5)
    return restarted
