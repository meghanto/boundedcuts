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
        backend = "embedded"
        if "--pb-sat-root-backend" in arguments:
            index = arguments.index("--pb-sat-root-backend")
            if index + 1 < len(arguments):
                backend = arguments[index + 1]
        if "--pb-sat-root-timeout" not in arguments:
            arguments.extend(("--pb-sat-root-timeout", "90"))
        if backend == "external":
            missing_solver = "--pb-sat-root-solver" not in arguments
            missing_checker = "--pb-sat-root-checker" not in arguments
            if missing_solver or missing_checker:
                bundled = proof_tools()
                if missing_solver:
                    arguments.extend(("--pb-sat-root-solver", bundled["cadical"]))
                if missing_checker:
                    arguments.extend(("--pb-sat-root-checker", bundled["drat_trim"]))
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
