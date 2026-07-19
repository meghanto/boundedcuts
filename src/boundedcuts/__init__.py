"""Python interface to the BoundedCuts exact cutwidth solver."""

from __future__ import annotations

from collections.abc import Iterable
from pathlib import Path
import json
import os
import tempfile
import numpy as np
import numpy.typing as npt

from ._boundedcuts import (
    Graph,
    SolveOptions,
    SolveResult,
    capabilities as _native_capabilities,
    solve as _native_solve,
)

__version__ = "0.1.0"


def proof_tools() -> dict[str, str]:
    """Return absolute paths to the proof tools bundled in this installation."""
    suffix = ".exe" if os.name == "nt" else ""
    directory = Path(__file__).resolve().parent / "bin"
    tools = {
        "cadical": directory / f"cadical{suffix}",
        "drat_trim": directory / f"drat-trim{suffix}",
    }
    missing = [str(path) for path in tools.values() if not path.is_file()]
    if missing:
        raise RuntimeError(f"bundled proof tools are missing: {missing}")
    return {name: str(path) for name, path in tools.items()}


def proof_tool_provenance() -> dict[str, object]:
    """Return the pinned source identity and licenses for bundled proof tools."""
    path = Path(__file__).resolve().with_name("proof_tools.json")
    return json.loads(path.read_text(encoding="utf-8"))


def capabilities() -> dict[str, bool]:
    """Report native solver features and bundled external proof tools."""
    available = dict(_native_capabilities())
    try:
        proof_tools()
    except RuntimeError:
        available["bundled_cadical"] = False
        available["bundled_drat_trim"] = False
    else:
        available["bundled_cadical"] = True
        available["bundled_drat_trim"] = True
    return available


def from_edges(
    edges: npt.ArrayLike,
    *,
    num_vertices: int | None = None,
    labels: Iterable[str] | None = None,
) -> Graph:
    """Build a graph from an integer ``(m, 2)`` edge array.

    A C-contiguous ``numpy.uint32`` input takes the native fast path without an
    intermediate edge-list allocation. Other integer arrays are normalized
    once before native graph construction.
    """
    array = np.asarray(edges)
    if array.size == 0 and array.ndim == 1:
        array = array.reshape(0, 2)
    if array.ndim != 2 or array.shape[1] != 2:
        raise ValueError(f"edges must have shape (m, 2), got {array.shape}")
    if array.size and not np.issubdtype(array.dtype, np.integer):
        raise TypeError("edge endpoints must have an integer dtype")
    if np.issubdtype(array.dtype, np.signedinteger) and np.any(array < 0):
        raise ValueError("edge endpoints must be non-negative")
    if array.size and int(array.max()) > np.iinfo(np.uint32).max:
        raise ValueError("edge endpoint exceeds the uint32 vertex range")

    if num_vertices is None:
        num_vertices = 0 if array.size == 0 else int(array.max()) + 1
    if isinstance(num_vertices, bool) or not isinstance(num_vertices, (int, np.integer)):
        raise TypeError("num_vertices must be an integer")
    num_vertices = int(num_vertices)
    if num_vertices < 0 or num_vertices > np.iinfo(np.uint32).max:
        raise ValueError("num_vertices is outside the uint32 vertex range")
    if array.size and int(array.max()) >= num_vertices:
        raise ValueError("edge endpoint is outside num_vertices")

    normalized = array
    if normalized.dtype != np.uint32 or not normalized.flags.c_contiguous:
        normalized = np.ascontiguousarray(normalized, dtype=np.uint32)
    native_labels = [] if labels is None else [str(label) for label in labels]
    return Graph(num_vertices, normalized, native_labels)


def solve(
    graph: Graph,
    *,
    options: SolveOptions | None = None,
    threads: int | None = None,
    time_limit: float | None = None,
    controller: str | None = None,
    memory_budget_bytes: int | None = None,
    adaptive_arms: Iterable[str] | None = None,
    verify: bool | None = None,
    pb_sat_root_solver: str | None = None,
    pb_sat_root_checker: str | None = None,
    pb_sat_root_dir: str | None = None,
    pb_sat_root_timeout: float | None = None,
    pb_sat_root_q: int | None = None,
    pb_sat_root_max_gap: int | None = None,
) -> SolveResult:
    """Solve ``graph`` while releasing the Python GIL during native search."""
    if not isinstance(graph, Graph):
        raise TypeError("graph must be a boundedcuts.Graph")
    owns_options = options is None
    if owns_options:
        options = SolveOptions()
    elif any(value is not None for value in (
        threads, time_limit, controller, memory_budget_bytes, adaptive_arms, verify,
        pb_sat_root_solver, pb_sat_root_checker, pb_sat_root_dir, pb_sat_root_timeout,
        pb_sat_root_q, pb_sat_root_max_gap
    )):
        raise TypeError("pass either options or individual solve keywords, not both")
    if threads is not None:
        options.threads = int(threads)
    if time_limit is not None:
        options.time_limit = float(time_limit)
    if controller is not None:
        options.controller = str(controller)
    if memory_budget_bytes is not None:
        options.memory_budget_bytes = int(memory_budget_bytes)
    if adaptive_arms is not None:
        options.adaptive_arms = [str(arm) for arm in adaptive_arms]
    if verify is not None:
        options.verify = bool(verify)
    if pb_sat_root_solver is not None:
        options.pb_sat_root_solver = str(pb_sat_root_solver)
    if pb_sat_root_checker is not None:
        options.pb_sat_root_checker = str(pb_sat_root_checker)
    if pb_sat_root_dir is not None:
        options.pb_sat_root_dir = str(pb_sat_root_dir)
    if pb_sat_root_timeout is not None:
        options.pb_sat_root_timeout = float(pb_sat_root_timeout)
    if pb_sat_root_q is not None:
        options.pb_sat_root_q = int(pb_sat_root_q)
    if pb_sat_root_max_gap is not None:
        options.pb_sat_root_max_gap = int(pb_sat_root_max_gap)

    temporary_proof_directory: tempfile.TemporaryDirectory[str] | None = None
    original_pb_values: tuple[str, str, str, float] | None = None
    if options.adaptive_arms and "pb-sat-root" in options.adaptive_arms:
        original_pb_values = (
            options.pb_sat_root_solver,
            options.pb_sat_root_checker,
            options.pb_sat_root_dir,
            options.pb_sat_root_timeout,
        )
        bundled = proof_tools()
        if not options.pb_sat_root_solver:
            options.pb_sat_root_solver = bundled["cadical"]
        if not options.pb_sat_root_checker:
            options.pb_sat_root_checker = bundled["drat_trim"]
        if not options.pb_sat_root_dir:
            temporary_proof_directory = tempfile.TemporaryDirectory(
                prefix="boundedcuts-pb-sat-"
            )
            options.pb_sat_root_dir = temporary_proof_directory.name
        if options.pb_sat_root_timeout <= 0.0:
            options.pb_sat_root_timeout = 90.0

    try:
        return _native_solve(graph, options)
    finally:
        if temporary_proof_directory is not None:
            temporary_proof_directory.cleanup()
        if original_pb_values is not None:
            (
                options.pb_sat_root_solver,
                options.pb_sat_root_checker,
                options.pb_sat_root_dir,
                options.pb_sat_root_timeout,
            ) = original_pb_values


__all__ = [
    "Graph",
    "SolveOptions",
    "SolveResult",
    "capabilities",
    "from_edges",
    "proof_tools",
    "proof_tool_provenance",
    "solve",
    "__version__",
]
