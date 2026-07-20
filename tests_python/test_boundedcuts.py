from __future__ import annotations

import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np
import pytest

import boundedcuts
from boundedcuts import cli as boundedcuts_cli


def test_uint32_contiguous_fast_path() -> None:
    edges = np.array([[0, 1], [1, 2]], dtype=np.uint32)
    graph = boundedcuts.from_edges(edges, num_vertices=3)
    assert graph.vertex_count == 3
    assert graph.edge_count == 2


def test_int64_noncontiguous_normalization() -> None:
    edges = np.array([[0, 1, 2], [1, 2, 0]], dtype=np.int64).T
    assert not edges.flags.c_contiguous
    graph = boundedcuts.from_edges(edges, num_vertices=3)
    assert graph.edge_count == 3


def test_duplicate_loops_and_isolated_vertices() -> None:
    edges = np.array([[0, 0], [0, 1], [0, 1], [1, 0]], dtype=np.uint32)
    graph = boundedcuts.from_edges(edges, num_vertices=5)
    assert graph.vertex_count == 5
    assert graph.edge_count == 1
    empty = boundedcuts.from_edges([], num_vertices=4)
    assert empty.vertex_count == 4
    assert empty.edge_count == 0


def test_triangle_solve_and_owned_numpy_result() -> None:
    edges = np.array([[0, 1], [1, 2], [2, 0]], dtype=np.uint32)
    graph = boundedcuts.from_edges(edges)
    result = boundedcuts.solve(graph)
    assert result.status == "OPTIMAL"
    assert result.optimal
    assert (result.lower_bound, result.upper_bound) == (2, 2)
    assert result.verified
    assert isinstance(result.ordering, np.ndarray)
    assert result.ordering.dtype == np.uint32
    assert result.ordering.flags.c_contiguous
    assert sorted(result.ordering.tolist()) == [0, 1, 2]
    ordering = result.ordering
    del result
    assert sorted(ordering.tolist()) == [0, 1, 2]


@pytest.mark.parametrize(
    "edges, error",
    [
        (np.array([[0, 1, 2]], dtype=np.uint32), ValueError),
        (np.array([[0, -1]], dtype=np.int32), ValueError),
        (np.array([[0.0, 1.0]]), TypeError),
        (np.array([[0, 2]], dtype=np.uint32), ValueError),
    ],
)
def test_input_validation(edges: np.ndarray, error: type[Exception]) -> None:
    with pytest.raises(error):
        boundedcuts.from_edges(edges, num_vertices=2)


def test_option_validation() -> None:
    graph = boundedcuts.from_edges([[0, 1]])
    with pytest.raises(ValueError):
        boundedcuts.solve(graph, threads=0)
    with pytest.raises(ValueError):
        boundedcuts.solve(graph, controller="mystery")


def test_capabilities_include_portable_bounds() -> None:
    available = boundedcuts.capabilities()
    assert available["dfs"]
    assert available["partial_bounds"]
    assert available["residual_dp"]
    assert available["lagrangian_bounds"]
    assert available["sdp_certificate_verifier"]
    assert available["pb_sat_root"]
    if sys.platform != "win32":
        assert available["bundled_cadical"]
        assert available["bundled_drat_trim"]


def test_bundled_proof_chain() -> None:
    if sys.platform == "win32" and not boundedcuts.capabilities().get("bundled_cadical"):
        pytest.skip("Bundled proof tool executables are omitted on Windows")
    tools = boundedcuts.proof_tools()
    provenance = boundedcuts.proof_tool_provenance()
    assert provenance["cadical"]["version"] == "2.1.3"
    assert len(provenance["cadical"]["commit"]) == 40
    assert len(provenance["drat_trim"]["commit"]) == 40
    version = subprocess.run(
        [tools["cadical"], "--version"], capture_output=True, text=True, check=True
    )
    assert version.stdout.strip() == "2.1.3"

    with tempfile.TemporaryDirectory(prefix="boundedcuts-proof-test-") as directory:
        root = Path(directory)
        cnf = root / "contradiction.cnf"
        proof = root / "proof.drat"
        cnf.write_text("p cnf 1 2\n1 0\n-1 0\n", encoding="ascii")
        solved = subprocess.run(
            [tools["cadical"], str(cnf), str(proof)],
            capture_output=True,
            text=True,
            check=False,
        )
        assert solved.returncode == 20
        assert proof.stat().st_size > 0
        checked = subprocess.run(
            [tools["drat_trim"], str(cnf), str(proof)],
            capture_output=True,
            text=True,
            check=False,
        )
        assert checked.returncode == 0
        assert "VERIFIED" in checked.stdout + checked.stderr

        # Byte 0x1a is interpreted as EOF by the Windows C runtime in text
        # mode. This valid binary DRAT catches a checker built without "rb".
        binary_proof = root / "binary-proof.drat"
        binary_proof.write_bytes(bytes((ord("a"), 0x1A, 0, ord("a"), 0)))
        binary_checked = subprocess.run(
            [tools["drat_trim"], str(cnf), str(binary_proof)],
            capture_output=True,
            text=True,
            check=False,
        )
        assert binary_checked.returncode == 0
        assert "VERIFIED" in binary_checked.stdout + binary_checked.stderr


def test_python_embedded_pb_sat_defaults(monkeypatch: pytest.MonkeyPatch) -> None:
    graph = boundedcuts.from_edges([[0, 1]])
    options = boundedcuts.SolveOptions()
    options.controller = "adaptive"
    options.adaptive_arms = ["dfs", "pb-sat-root"]
    captured: dict[str, object] = {}
    sentinel = object()

    def fake_solve(native_graph, native_options):
        assert native_graph is graph
        captured["solver"] = native_options.pb_sat_root_solver
        captured["checker"] = native_options.pb_sat_root_checker
        captured["directory"] = native_options.pb_sat_root_dir
        captured["timeout"] = native_options.pb_sat_root_timeout
        assert native_options.pb_sat_root_backend == "embedded"
        assert native_options.pb_sat_root_ordering == "auto"
        return sentinel

    monkeypatch.setattr(boundedcuts, "_native_solve", fake_solve)
    assert boundedcuts.solve(graph, options=options) is sentinel
    assert captured["solver"] == ""
    assert captured["checker"] == ""
    assert captured["directory"] == ""
    assert captured["timeout"] == 0.0
    assert options.pb_sat_root_solver == ""
    assert options.pb_sat_root_checker == ""
    assert options.pb_sat_root_dir == ""
    assert options.pb_sat_root_timeout == 0.0


def test_cli_invocation() -> None:
    completed = subprocess.run(
        [sys.executable, "-m", "boundedcuts", "--help"],
        capture_output=True,
        text=True,
        check=False,
    )
    assert completed.returncode == 0
    assert "Usage:" in completed.stdout


@pytest.mark.parametrize(
    ("backend_arguments", "expected_timeout"),
    [
        ([], None),
        (["--pb-sat-root-backend", "external"], "90"),
        (["--pb-sat-root-timeout", "17"], "17"),
    ],
)
def test_cli_pb_timeout_defaults_follow_backend(
    monkeypatch: pytest.MonkeyPatch,
    backend_arguments: list[str],
    expected_timeout: str | None,
) -> None:
    captured: list[str] = []

    class ExecCalled(Exception):
        pass

    def fake_execv(_path: str, command: list[str]) -> None:
        captured.extend(command)
        raise ExecCalled

    def fake_run(command: list[str], *, check: bool) -> subprocess.CompletedProcess[str]:
        assert not check
        captured.extend(command)
        return subprocess.CompletedProcess(command, 0)

    monkeypatch.setattr(boundedcuts_cli, "executable", lambda: Path("/fake/cutwidth_exact"))
    monkeypatch.setattr(boundedcuts_cli.os, "execv", fake_execv)
    monkeypatch.setattr(boundedcuts_cli.subprocess, "run", fake_run)
    monkeypatch.setattr(
        sys,
        "argv",
        [
            "boundedcuts",
            "graph.edgelist",
            "--adaptive-arms",
            "dfs,pb-sat-root",
            *backend_arguments,
            "--pb-sat-root-solver",
            "solver",
            "--pb-sat-root-checker",
            "checker",
            "--pb-sat-root-dir",
            "proofs",
        ],
    )

    try:
        assert boundedcuts_cli.main() == 0
    except ExecCalled:
        pass

    if expected_timeout is None:
        assert "--pb-sat-root-timeout" not in captured
    else:
        index = captured.index("--pb-sat-root-timeout")
        assert captured[index + 1] == expected_timeout


def test_cli_defaults_to_embedded_proof_tools(tmp_path: Path) -> None:
    graph = tmp_path / "triangle.edgelist"
    graph.write_text("0 1\n1 2\n2 0\n", encoding="ascii")
    completed = subprocess.run(
        [
            sys.executable,
            "-m",
            "boundedcuts",
            str(graph),
            "--json",
            "--verify",
            "--engine",
            "v2",
            "--threads",
            "2",
            "--time-limit",
            "1",
            "--controller",
            "adaptive",
            "--adaptive-arms",
            "dfs,pb-sat-root",
            "--pb-sat-root-max-gap",
            "32",
        ],
        capture_output=True,
        text=True,
        check=False,
    )
    assert completed.returncode == 0, completed.stderr
    assert '"verified":true' in completed.stdout


def test_embedded_unsat_validation() -> None:
    # A simple triangle has cutwidth 2. Solving with threshold 1 is UNSAT.
    edges = np.array([[0, 1], [1, 2], [2, 0]], dtype=np.uint32)
    graph = boundedcuts.from_edges(edges)
    options = boundedcuts.SolveOptions()
    options.controller = "adaptive"
    options.adaptive_arms = ["dfs", "pb-sat-root"]
    options.pb_sat_root_backend = "embedded"
    options.pb_sat_root_max_gap = 10
    options.pb_sat_root_timeout = 5.0

    result = boundedcuts.solve(graph, options=options)
    assert result.optimal
    assert result.lower_bound == 2
    assert result.upper_bound == 2


def test_sat_witness_verification() -> None:
    # A path graph on 3 vertices has cutwidth 1. Solving with threshold 1 is SAT.
    edges = np.array([[0, 1], [1, 2]], dtype=np.uint32)
    graph = boundedcuts.from_edges(edges)
    options = boundedcuts.SolveOptions()
    options.controller = "adaptive"
    options.adaptive_arms = ["dfs", "pb-sat-root"]
    options.pb_sat_root_backend = "embedded"
    options.pb_sat_root_max_gap = 10
    options.pb_sat_root_timeout = 5.0

    result = boundedcuts.solve(graph, options=options)
    assert result.optimal
    assert result.lower_bound == 1
    assert result.upper_bound == 1


def test_backend_validation() -> None:
    edges = np.array([[0, 1]], dtype=np.uint32)
    graph = boundedcuts.from_edges(edges)
    options = boundedcuts.SolveOptions()
    options.controller = "adaptive"
    options.adaptive_arms = ["dfs", "pb-sat-root"]
    options.pb_sat_root_backend = "invalid_backend"
    options.pb_sat_root_timeout = 1.0
    with pytest.raises(ValueError):
        boundedcuts.solve(graph, options=options)


def test_pb_root_ordering_validation() -> None:
    graph = boundedcuts.from_edges([[0, 1]])
    options = boundedcuts.SolveOptions()
    options.controller = "adaptive"
    options.adaptive_arms = ["dfs", "pb-sat-root"]
    options.pb_sat_root_ordering = "not-an-ordering"
    with pytest.raises(ValueError):
        boundedcuts.solve(graph, options=options)


def test_windows_dependency_check() -> None:
    if sys.platform != "win32":
        pytest.skip("Windows dependency check only runs on Windows")
    # Inspect every shipped Windows binary, not only the Python extension.
    package_dir = Path(boundedcuts.__file__).parent
    pyd_files = list(package_dir.rglob("*.pyd"))
    assert len(pyd_files) > 0, "No .pyd extension found"
    pe_files = pyd_files + list(package_dir.rglob("*.exe")) + list(package_dir.rglob("*.dll"))

    forbidden = [b"libgcc", b"libstdc++", b"libwinpthread", b"msys-"]
    for pe_file in pe_files:
        content = pe_file.read_bytes().lower()
        for runtime in forbidden:
            assert runtime not in content, (
                f"Forbidden non-MSVC runtime {runtime.decode()} found in {pe_file.name}"
            )


def test_sdp_mapping_and_telemetry() -> None:
    edges = np.array([[0, 1], [1, 2]], dtype=np.uint32)
    graph = boundedcuts.from_edges(edges)
    options = boundedcuts.SolveOptions()
    options.controller = "adaptive"
    options.adaptive_arms = ["dfs", "sdp"]

    result = boundedcuts.solve(graph, options=options)

    # Verify the telemetry properties are exposed
    assert hasattr(result, "sdp_attempted")
    assert hasattr(result, "sdp_available")
    assert hasattr(result, "sdp_raw_converged")
    assert hasattr(result, "sdp_primal_residual")
    assert hasattr(result, "sdp_certified_lower_bound")
    assert hasattr(result, "sdp_primal_objective")
    assert hasattr(result, "sdp_dual_objective")
    assert hasattr(result, "sdp_dual_residual")
    assert hasattr(result, "sdp_solve_seconds")
    assert hasattr(result, "sdp_solver_iterations")
    assert hasattr(result, "sdp_solver_status")
    assert hasattr(result, "sdp_bisection_calls")
    assert hasattr(result, "sdp_triangle_cuts")
    assert hasattr(result, "sdp_state_requests")
    assert hasattr(result, "sdp_state_certified")
    assert hasattr(result, "sdp_state_prunes")
    assert hasattr(result, "sdp_state_cache_hits")
    assert hasattr(result, "sdp_state_calls")
    assert hasattr(result, "sdp_state_busy")
    assert hasattr(result, "sdp_state_budget_rejections")
    assert hasattr(result, "sdp_state_uncertified")
    assert hasattr(result, "sdp_state_dimension_rejections")
    assert hasattr(result, "sdp_state_preferred_max_dimension")

    assert result.status in ("OPTIMAL", "FEASIBLE")


def test_sdp_budget_defaults_and_explicit_unlimited_controls() -> None:
    options = boundedcuts.SolveOptions()
    assert options.sdp_total_time == 5.0
    assert options.sdp_max_calls == 2
    assert options.sdp_max_state_dimension == 64

    # Zero is deliberately available to callers who want the native unlimited
    # oracle semantics.  The actual oracle remains opt-in through the arm.
    options.sdp_total_time = 0.0
    options.sdp_max_calls = 0
    options.sdp_max_state_dimension = 0
    options.controller = "adaptive"
    options.adaptive_arms = ["dfs", "sdp"]
    graph = boundedcuts.from_edges(np.array([[0, 1]], dtype=np.uint32))
    result = boundedcuts.solve(graph, options=options)
    assert result.status == "OPTIMAL"


def test_sdp_disabled_by_default() -> None:
    edges = np.array([[0, 1]], dtype=np.uint32)
    graph = boundedcuts.from_edges(edges)

    # 1. Default solve (static controller, sdp is absent/not run as adaptive arm)
    result_default = boundedcuts.solve(graph)
    assert not result_default.sdp_attempted

    # 2. Adaptive controller but sdp is absent from adaptive_arms
    options_no_sdp = boundedcuts.SolveOptions()
    options_no_sdp.controller = "adaptive"
    options_no_sdp.adaptive_arms = ["dfs", "bounds"]
    result_no_sdp = boundedcuts.solve(graph, options=options_no_sdp)
    assert not result_no_sdp.sdp_attempted


def test_sdp_capability_safe_fallback() -> None:
    edges = np.array([[0, 1], [1, 2], [2, 0]], dtype=np.uint32)
    graph = boundedcuts.from_edges(edges)
    options = boundedcuts.SolveOptions()
    options.controller = "adaptive"
    options.adaptive_arms = ["dfs", "sdp"]

    capabilities = boundedcuts.capabilities()
    has_clarabel = capabilities.get("clarabel", False)

    result = boundedcuts.solve(graph, options=options)

    # If clarabel is not compiled in, the solver must fall back safely.
    assert result.sdp_available == has_clarabel
    if not has_clarabel:
        assert result.sdp_certified_lower_bound is None


def test_clarabel_capability_and_packaging() -> None:
    capabilities = boundedcuts.capabilities()
    if sys.platform == "win32":
        # On Windows, the wheel build MUST include Clarabel SDP capability
        assert capabilities.get("clarabel"), "Clarabel SDP backend must be enabled on Windows"
        # Check that the packaged library and provenance are present
        package_dir = Path(boundedcuts.__file__).parent
        dlls = list(package_dir.rglob("clarabel_c.dll"))
        assert len(dlls) > 0, "clarabel_c.dll must be packaged beside the extension on Windows"

        licenses = list(package_dir.rglob("Clarabel-LICENSE.txt"))
        assert len(licenses) > 0, "Clarabel license must be packaged beside the extension on Windows"

        provenance = list(package_dir.rglob("clarabel_provenance.txt"))
        assert len(provenance) > 0, "Clarabel provenance must be packaged beside the extension on Windows"
