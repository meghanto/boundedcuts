"""Python interface to the BoundedCuts exact cutwidth solver."""

from __future__ import annotations

from collections.abc import Iterable
import numpy as np
import numpy.typing as npt

from ._boundedcuts import (
    Graph,
    SolveOptions,
    SolveResult,
    capabilities,
    solve as _native_solve,
)

__version__ = "0.1.0"


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
) -> SolveResult:
    """Solve ``graph`` while releasing the Python GIL during native search."""
    if not isinstance(graph, Graph):
        raise TypeError("graph must be a boundedcuts.Graph")
    if options is None:
        options = SolveOptions()
    elif any(value is not None for value in (
        threads, time_limit, controller, memory_budget_bytes, adaptive_arms, verify
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
    return _native_solve(graph, options)


__all__ = [
    "Graph",
    "SolveOptions",
    "SolveResult",
    "capabilities",
    "from_edges",
    "solve",
    "__version__",
]
