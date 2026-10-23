#!/usr/bin/python3
"""Run host awk on LF views of text from the upstream Windows source export."""
from pathlib import Path
import subprocess
import sys
import tempfile
with tempfile.TemporaryDirectory(prefix='lsx4-awk-') as folder:
    arguments=[]
    for index,argument in enumerate(sys.argv[1:]):
        path=Path(argument)
        if path.is_file():
            data=path.read_bytes()
            if b'\r\n' in data:
                normalized=Path(folder)/str(index)/path.name
                normalized.parent.mkdir()
                normalized.write_bytes(data.replace(b'\r\n',b'\n'))
                argument=str(normalized)
        arguments.append(argument)
    sys.exit(subprocess.run(['/usr/bin/awk',*arguments]).returncode)
