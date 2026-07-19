"""Console entry point for the bundled native BoundedCuts executable."""

from __future__ import annotations

import os
from pathlib import Path
import subprocess
import sys
import tempfile

from . import proof_tools


def executable() -> Path:
    name = "cutwidth_exact.exe" if os.name == "nt" else "cutwidth_exact"
    path = Path(__file__).resolve().parent / name
    if not path.is_file():
        raise RuntimeError(f"bundled BoundedCuts executable is missing: {path}")
    return path


def main() -> int:
    arguments = list(sys.argv[1:])
    temporary_proof_directory: tempfile.TemporaryDirectory[str] | None = None
    arms = ""
    if "--adaptive-arms" in arguments:
        index = arguments.index("--adaptive-arms")
        if index + 1 < len(arguments):
            arms = arguments[index + 1]
    if "pb-sat-root" in {arm.strip() for arm in arms.split(",")}:
        bundled = proof_tools()
        defaults = {
            "--pb-sat-root-solver": bundled["cadical"],
            "--pb-sat-root-checker": bundled["drat_trim"],
            "--pb-sat-root-timeout": "90",
        }
        for flag, value in defaults.items():
            if flag not in arguments:
                arguments.extend((flag, value))
        if "--pb-sat-root-dir" not in arguments:
            temporary_proof_directory = tempfile.TemporaryDirectory(
                prefix="boundedcuts-pb-sat-"
            )
            arguments.extend((
                "--pb-sat-root-dir",
                temporary_proof_directory.name,
            ))
    command = [str(executable()), *arguments]
    if os.name != "nt" and temporary_proof_directory is None:
        os.execv(command[0], command)
        raise AssertionError("os.execv returned unexpectedly")
    try:
        return subprocess.run(command, check=False).returncode
    finally:
        if temporary_proof_directory is not None:
            temporary_proof_directory.cleanup()
