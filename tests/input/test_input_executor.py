#!/usr/bin/env python3
"""Compile and exercise the production serial input executor without platform dependencies."""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compiler', default='g++')
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='input-executor-') as temporary:
        executable = Path(temporary) / 'test.exe'
        subprocess.run([args.compiler, '-std=c++20', '-pthread', '-Wall', '-Wextra', '-Werror',
                        '-I', str(ROOT), str(Path(__file__).with_suffix('.cpp')), '-o', str(executable)],
                       check=True)
        subprocess.run([str(executable)], check=True, timeout=15)


if __name__ == '__main__':
    main()
