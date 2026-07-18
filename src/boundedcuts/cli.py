"""Console entry point for the bundled native BoundedCuts executable."""

from __future__ import annotations

import os
from pathlib import Path
import subprocess
import sys


def executable() -> Path:
    name = "cutwidth_exact.exe" if os.name == "nt" else "cutwidth_exact"
    path = Path(__file__).resolve().parent / name
    if not path.is_file():
        raise RuntimeError(f"bundled BoundedCuts executable is missing: {path}")
    return path


def main() -> int:
    command = [str(executable()), *sys.argv[1:]]
    if os.name != "nt":
        os.execv(command[0], command)
        raise AssertionError("os.execv returned unexpectedly")
    return subprocess.call(command)
