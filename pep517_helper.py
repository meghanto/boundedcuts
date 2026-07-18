"""PEP 517 wrapper that restores pinned C++ dependencies with Conan."""

from __future__ import annotations

from contextlib import contextmanager
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

from scikit_build_core import build as _backend


_ROOT = Path(__file__).resolve().parent


@contextmanager
def _native_build_environment():
    conan = shutil.which("conan")
    if conan is None:
        raise RuntimeError("Conan 2.30.0 is required to build BoundedCuts from source")

    with tempfile.TemporaryDirectory(prefix="boundedcuts-conan-") as temporary:
        temporary_path = Path(temporary)
        conan_home = Path(os.environ.get("CONAN_HOME", temporary_path / "home"))
        output = temporary_path / "build"
        environment = os.environ.copy()
        environment["CONAN_HOME"] = str(conan_home)

        subprocess.run(
            [conan, "profile", "detect", "--force"],
            cwd=_ROOT,
            env=environment,
            check=True,
        )
        subprocess.run(
            [
                conan,
                "install",
                str(_ROOT / "exact_solver"),
                "--build=missing",
                "--settings",
                "build_type=Release",
                "--output-folder",
                str(output),
            ],
            cwd=_ROOT,
            env=environment,
            check=True,
        )

        toolchains = list(output.rglob("conan_toolchain.cmake"))
        if len(toolchains) != 1:
            raise RuntimeError(
                f"expected one Conan toolchain, found {len(toolchains)} under {output}"
            )

        baseline = [
            f"-DCMAKE_TOOLCHAIN_FILE:FILEPATH={toolchains[0]}",
            "-DCUTWIDTH_BUILD_PYTHON=ON",
            "-DCUTWIDTH_BUILD_TESTS=OFF",
            "-DCMAKE_CONFIGURATION_TYPES=Release",
            "-DCUTWIDTH_ENABLE_HIGHS=OFF",
            "-DCUTWIDTH_ENABLE_ONETBB=OFF",
            "-DCUTWIDTH_ENABLE_SDP_PROTOTYPE=OFF",
        ]
        previous_args = os.environ.get("SKBUILD_CMAKE_ARGS")
        previous_conan_home = os.environ.get("CONAN_HOME")
        if previous_args:
            baseline.extend(argument for argument in previous_args.split(";") if argument)
        os.environ["SKBUILD_CMAKE_ARGS"] = ";".join(baseline)
        os.environ["CONAN_HOME"] = str(conan_home)
        try:
            yield
        finally:
            if previous_args is None:
                os.environ.pop("SKBUILD_CMAKE_ARGS", None)
            else:
                os.environ["SKBUILD_CMAKE_ARGS"] = previous_args
            if previous_conan_home is None:
                os.environ.pop("CONAN_HOME", None)
            else:
                os.environ["CONAN_HOME"] = previous_conan_home


def build_wheel(*args, **kwargs):
    with _native_build_environment():
        return _backend.build_wheel(*args, **kwargs)


def build_editable(*args, **kwargs):
    with _native_build_environment():
        return _backend.build_editable(*args, **kwargs)


build_sdist = _backend.build_sdist
get_requires_for_build_wheel = _backend.get_requires_for_build_wheel
get_requires_for_build_editable = _backend.get_requires_for_build_editable
get_requires_for_build_sdist = _backend.get_requires_for_build_sdist
prepare_metadata_for_build_wheel = _backend.prepare_metadata_for_build_wheel
prepare_metadata_for_build_editable = _backend.prepare_metadata_for_build_editable
