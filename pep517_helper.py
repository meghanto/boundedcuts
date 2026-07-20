"""PEP 517 wrapper that restores pinned C++ dependencies with Conan."""

from __future__ import annotations

from contextlib import contextmanager
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

from scikit_build_core import build as _backend


_ROOT = Path(__file__).resolve().parent


def _bundled_proof_tools(temporary_path: Path, environment: dict[str, str]):
    destination = Path(
        environment.get("BOUNDEDCUTS_PB_ROOT", temporary_path / "pb-sat")
    ).resolve()
    jobs = str(max(1, os.cpu_count() or 1))

    cadical_root = destination / "cadical"
    checker_root = destination / "drat-trim"

    def clone_pin(url, commit, dir_path):
        dir_path.mkdir(parents=True, exist_ok=True)
        if not (dir_path / ".git").is_dir():
            subprocess.run(["git", "clone", "--filter=blob:none", url, str(dir_path)], check=True)
        try:
            current = subprocess.run(
                ["git", "-C", str(dir_path), "rev-parse", "HEAD"],
                capture_output=True, text=True, check=True
            ).stdout.strip()
            if current == commit:
                return
        except Exception:
            pass
        subprocess.run(["git", "-C", str(dir_path), "fetch", "--quiet", "origin", commit], check=True)
        subprocess.run(["git", "-C", str(dir_path), "checkout", "--detach", "--quiet", commit], check=True)

    cadical_commit = "f13d74439a5b5c963ac5b02d05ce93a8098018b8"
    drat_trim_commit = "2e3b2dc0ecf938addbd779d42877b6ed69d9a985"

    clone_pin("https://github.com/arminbiere/cadical.git", cadical_commit, cadical_root)
    clone_pin("https://github.com/marijnheule/drat-trim.git", drat_trim_commit, checker_root)

    if os.name == "nt":
        paths = {
            "cadical": None,
            "checker": None,
            "cadical_root": cadical_root,
            "checker_root": checker_root,
            "cadical_license": cadical_root / "LICENSE",
            "checker_license": checker_root / "LICENSE",
        }
    else:
        subprocess.run(
            ["bash", str(_ROOT / "tools" / "bootstrap_wheel_pb_sat.sh"),
             str(destination), jobs],
            cwd=_ROOT,
            env=environment,
            check=True,
        )
        paths = {
            "cadical": cadical_root / "build" / "cadical",
            "checker": checker_root / "drat-trim",
            "cadical_root": cadical_root,
            "checker_root": checker_root,
            "cadical_license": cadical_root / "LICENSE",
            "checker_license": checker_root / "LICENSE",
        }

    missing = [str(path) for path in [paths["cadical_license"], paths["checker_license"]] if not path.is_file()]
    if os.name != "nt":
        missing.extend(str(paths[key]) for key in ["cadical", "checker"] if not paths[key].is_file())
    if missing:
        raise RuntimeError(f"bundled proof-tool artifacts or licenses are missing: {missing}")
    return paths


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
        if sys.platform == "darwin":
            environment.setdefault("MACOSX_DEPLOYMENT_TARGET", "11.0")

        proof_tools = _bundled_proof_tools(temporary_path, environment)

        # On Windows: fetch/pin Clarabel.cpp, set conan option with_sdp=True
        conan_options = ["boost/*:header_only=True"]
        clarabel_root = None
        if os.name == "nt":
            conan_options.append("with_sdp=True")
            clarabel_root = temporary_path / "clarabel"
            clarabel_root.mkdir(parents=True, exist_ok=True)
            subprocess.run(["git", "clone", "--filter=blob:none", "https://github.com/oxfordcontrol/Clarabel.cpp.git", str(clarabel_root)], check=True)
            subprocess.run(["git", "-C", str(clarabel_root), "fetch", "--quiet", "origin", "0de6259a3edfd5cc041ec42b2148599ce63e73cb"], check=True)
            subprocess.run(["git", "-C", str(clarabel_root), "checkout", "--detach", "--quiet", "0de6259a3edfd5cc041ec42b2148599ce63e73cb"], check=True)
            subprocess.run(["git", "-C", str(clarabel_root), "submodule", "update", "--init", "--recursive"], check=True)

        subprocess.run(
            [conan, "profile", "detect", "--force"],
            cwd=_ROOT,
            env=environment,
            check=True,
        )

        conan_args = [
            conan,
            "install",
            str(_ROOT / "exact_solver"),
            "--build=missing",
            "--settings",
            "build_type=Release",
            *(
                ["--settings", "os.version=" + environment["MACOSX_DEPLOYMENT_TARGET"]]
                if sys.platform == "darwin"
                else []
            ),
        ]
        for opt in conan_options:
            conan_args.extend(["--options", opt])
        conan_args.extend(["--output-folder", str(output)])

        subprocess.run(
            conan_args,
            cwd=_ROOT,
            env=environment,
            check=True,
        )

        # On Windows: bootstrap Clarabel using powershell script
        if os.name == "nt":
            openblas_libs = list(conan_home.glob("**/openblas.lib"))
            if not openblas_libs:
                raise RuntimeError("Could not find openblas.lib under Conan home")
            openblas_lib_path = openblas_libs[0]

            ps_script = _ROOT / "tools" / "bootstrap_clarabel_windows.ps1"
            subprocess.run(
                [
                    "powershell.exe",
                    "-NoProfile",
                    "-ExecutionPolicy",
                    "Bypass",
                    "-File",
                    str(ps_script),
                    str(clarabel_root),
                    str(openblas_lib_path),
                    str(temporary_path / "clarabel_build"),
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

        enable_sdp = "ON" if os.name == "nt" else "OFF"
        baseline = [
            f"-DCMAKE_TOOLCHAIN_FILE:FILEPATH={toolchains[0]}",
            "-DCUTWIDTH_BUILD_PYTHON=ON",
            "-DCUTWIDTH_BUILD_TESTS=OFF",
            "-DCMAKE_CONFIGURATION_TYPES=Release",
            "-DCUTWIDTH_ENABLE_HIGHS=OFF",
            "-DCUTWIDTH_ENABLE_ONETBB=OFF",
            f"-DCUTWIDTH_ENABLE_SDP_PROTOTYPE={enable_sdp}",
            f"-DCUTWIDTH_BUNDLED_CADICAL_LICENSE:FILEPATH={proof_tools['cadical_license']}",
            f"-DCUTWIDTH_BUNDLED_DRAT_TRIM_LICENSE:FILEPATH={proof_tools['checker_license']}",
            f"-DCUTWIDTH_CADICAL_ROOT:PATH={proof_tools['cadical_root']}",
            f"-DCUTWIDTH_DRAT_TRIM_ROOT:PATH={proof_tools['checker_root']}",
        ]
        if clarabel_root is not None:
            baseline.append(f"-DCUTWIDTH_CLARABEL_ROOT:PATH={clarabel_root}")
        if proof_tools["cadical"] is not None:
            baseline.extend((
                f"-DCUTWIDTH_BUNDLED_CADICAL:FILEPATH={proof_tools['cadical']}",
                f"-DCUTWIDTH_BUNDLED_DRAT_TRIM:FILEPATH={proof_tools['checker']}",
            ))
        if sys.platform == "darwin":
            baseline.append(
                "-DCMAKE_OSX_DEPLOYMENT_TARGET:STRING="
                + environment["MACOSX_DEPLOYMENT_TARGET"]
            )
        previous_args = os.environ.get("SKBUILD_CMAKE_ARGS")
        previous_conan_home = os.environ.get("CONAN_HOME")
        previous_macos_target = os.environ.get("MACOSX_DEPLOYMENT_TARGET")
        if previous_args:
            baseline.extend(argument for argument in previous_args.split(";") if argument)
        os.environ["SKBUILD_CMAKE_ARGS"] = ";".join(baseline)
        os.environ["CONAN_HOME"] = str(conan_home)
        if sys.platform == "darwin":
            os.environ["MACOSX_DEPLOYMENT_TARGET"] = environment[
                "MACOSX_DEPLOYMENT_TARGET"
            ]
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
            if sys.platform == "darwin":
                if previous_macos_target is None:
                    os.environ.pop("MACOSX_DEPLOYMENT_TARGET", None)
                else:
                    os.environ["MACOSX_DEPLOYMENT_TARGET"] = previous_macos_target


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
