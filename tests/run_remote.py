#!/usr/bin/env python3
"""Run the command-file host tests (remote.c), without a game process."""
import argparse
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default=os.environ.get("CC", "gcc"))
    args = parser.parse_args()
    out = ROOT / "obj" / "tests"
    out.mkdir(parents=True, exist_ok=True)
    exe = out / ("remote" + (".exe" if os.name == "nt" else ""))
    for command in (
        [args.cc, "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
         "-Isrc/ootgz", "tests/remote.c", "-o", str(exe)],
        [str(exe)],
    ):
        result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)
        sys.stdout.write(result.stdout)
        sys.stderr.write(result.stderr)
        if result.returncode:
            sys.exit(result.returncode)


if __name__ == "__main__":
    main()
