from __future__ import annotations

import subprocess
import sys

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


def test_cli_invocation() -> None:
    completed = subprocess.run(
        [sys.executable, "-m", "boundedcuts", "--help"],
        capture_output=True,
        text=True,
        check=False,
    )
    assert completed.returncode == 0
    assert "Usage:" in completed.stdout
