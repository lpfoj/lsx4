"""Host C++20 compiler for native lifecycle execution tests."""
import os
from pathlib import Path
import shutil, subprocess, unittest

def compiler_command():
    for name in ('c++', 'g++', 'clang++'):
        if compiler := shutil.which(name):
            return [compiler, '-std=c++20', '-O2'], os.environ.copy(), False
    vcvars = Path(os.environ.get('ProgramFiles', 'C:/Program Files')) / (
        'Microsoft Visual Studio/2022/Community/VC/Auxiliary/Build/vcvars64.bat'
    )
    if os.name == 'nt' and vcvars.is_file():
        result = subprocess.run(
            f'"{os.environ.get("COMSPEC", "cmd.exe")}" /d /s /c ""{vcvars}" >nul && set"',
            check=True, capture_output=True, text=True, encoding='mbcs'
        )
        env = os.environ.copy()
        for line in result.stdout.splitlines():
            if '=' in line and not line.startswith('='):
                key, value = line.split('=', 1)
                env[key.upper()] = value
        if compiler := shutil.which('cl.exe', path=env.get('PATH')):
            return [compiler, '/nologo', '/std:c++20', '/EHsc', '/O2'], env, True
    raise unittest.SkipTest('A host C++20 compiler is required')

