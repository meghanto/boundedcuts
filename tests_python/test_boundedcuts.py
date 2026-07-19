from __future__ import annotations

import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np
import pytest

import boundedcuts


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
    assert available["bundled_cadical"]
    assert available["bundled_drat_trim"]


def test_bundled_proof_chain() -> None:
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


def test_python_pb_sat_defaults_and_cleanup(monkeypatch: pytest.MonkeyPatch) -> None:
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
        assert Path(native_options.pb_sat_root_dir).is_dir()
        return sentinel

    monkeypatch.setattr(boundedcuts, "_native_solve", fake_solve)
    assert boundedcuts.solve(graph, options=options) is sentinel
    bundled = boundedcuts.proof_tools()
    assert captured["solver"] == bundled["cadical"]
    assert captured["checker"] == bundled["drat_trim"]
    assert captured["timeout"] == 90.0
    assert not Path(str(captured["directory"])).exists()
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


def test_cli_auto_discovers_bundled_proof_tools(tmp_path: Path) -> None:
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
            "--pb-sat-root-dir",
            str(tmp_path / "proof"),
            "--pb-sat-root-max-gap",
            "32",
        ],
        capture_output=True,
        text=True,
        check=False,
    )
    assert completed.returncode == 0, completed.stderr
    assert '"verified":true' in completed.stdout
