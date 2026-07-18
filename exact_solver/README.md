# BoundedCuts command-line solver

BoundedCuts is a correctness-first C++20 solver for the cutwidth problem. Given
an undirected graph, it finds a vertex ordering that minimizes the largest cut
between a prefix and the remaining vertices.

A completed optimization reports `OPTIMAL` only when its certified lower and
upper bounds match. A time-limited run reports `FEASIBLE`, its current interval,
and a verified ordering; it does not claim optimality.

## Build

The default build requires CMake 3.20+, a C++20 compiler, Python 3, and Conan 2.
From the repository root:

```sh
python3 -m pip install "conan>=2.30,<3"
conan profile detect --force
cd exact_solver
conan install . --build=missing -s build_type=Release
```

On Linux and macOS:

```sh
cmake --preset conan-release -DCUTWIDTH_ENABLE_HIGHS=OFF
cmake --build --preset conan-release --parallel
ctest --test-dir build/Release --output-on-failure
```

On Windows:

```powershell
cmake --preset conan-default -DCUTWIDTH_ENABLE_HIGHS=OFF
cmake --build --preset conan-release --parallel
ctest --test-dir build -C Release --output-on-failure
```

Set `CUTWIDTH_BUILD_TESTS=OFF` at configure time to build only the solver.

## Input

Input is a whitespace-separated undirected edge list, one edge per line:

```text
# comments and blank lines are allowed
alice bob
bob carol
carol alice
isolated_vertex
```

Vertex labels are arbitrary strings and are preserved in the result. Self-loops
are ignored and duplicate edges are deduplicated. A line containing one label
declares an isolated vertex. Input can be a file, standard input, or the
positional filename `-`.

The solver also accepts the bundled-instance form `name: ...`, followed by an
`n m` header and `m` numeric edges.

## Usage

The executable is `build/Release/cutwidth_exact` on Linux/macOS and
`build\Release\cutwidth_exact.exe` on Windows when invoked from
`exact_solver/`.

```sh
# Prove the optimum.
build/Release/cutwidth_exact graph.edgelist --verify

# Use parallel search and stop after five minutes.
build/Release/cutwidth_exact graph.edgelist \
  --threads 16 --time-limit 300 --json --verify

# Decide whether cutwidth at most 15 is feasible.
build/Release/cutwidth_exact graph.edgelist \
  --max-width 15 --threads 16 --verify

# Read from standard input.
cat graph.edgelist | build/Release/cutwidth_exact --json
```

Common options:

- `-i, --input FILE` selects an input file.
- `-t, --time-limit SECONDS` sets a wall-clock limit; zero means unlimited.
- `-k, --max-width K` switches to the threshold decision problem.
- `--threads N` sets the maximum worker count.
- `--cache-memory BYTES` bounds proof-cache memory; zero means unlimited.
- `--controller static|adaptive` selects the optimization controller.
- `--checkpoint-out FILE` writes an unresolved adaptive search at its time
  limit; `--resume FILE` restores it.
- `--json` emits one machine-readable JSON object.
- `--verify` independently checks the returned ordering and cutwidth.
- `--help` lists every search and optional-backend control.

## Checkpoint and resume

Adaptive search can preserve all live threshold sessions when a time limit is
reached:

```sh
build/Release/cutwidth_exact graph.edgelist \
  --controller adaptive --threads 16 --time-limit 300 \
  --checkpoint-out search.ckpt --json --verify

build/Release/cutwidth_exact graph.edgelist \
  --controller adaptive --threads 16 --time-limit 300 \
  --resume search.ckpt --checkpoint-out search.ckpt --json --verify
```

Resume validates the graph and all search-policy compatibility fields before
accepting a checkpoint. The checkpoint is written atomically only when the
adaptive run reaches its configured limit with an unresolved interval.

## Output and exactness

Human output includes status, certified bounds, ordering, runtime, and search
statistics. JSON uses schema version 3 and exposes the same proof status plus
parallelism, cache, checkpoint, and optional-backend telemetry.

For optimization:

- `OPTIMAL` means `lower_bound == upper_bound` and the ordering attains that
  value.
- `FEASIBLE` means the ordering is valid but the certified interval remains
  open.

For `--max-width K` decisions:

- `FEASIBLE` includes a verified witness of width at most `K`.
- `INFEASIBLE` means the exact proof search ruled out every ordering.
- `UNKNOWN` means the run ended before either proof completed.

Floating-point heuristic, MILP, and SDP values are never accepted directly as
exact proofs. They can affect certified bounds only through the solver's exact
verification or certificate-recovery paths.

## Optional backends

The default binary includes combinatorial partial bounds, residual DP,
Lagrangian bounds, the SDP formulation, and exact certificate verification,
requiring only Boost and Dispenso from Conan. External numerical solvers and
proof-producing SAT backends remain optional:

- HiGHS can provide a root MILP oracle when its headers and library are
  discoverable at configure time. Use `CUTWIDTH_ENABLE_HIGHS=OFF` to disable
  detection explicitly.
- The Clarabel SDP solving adapter requires `CUTWIDTH_ENABLE_SDP_PROTOTYPE=ON`,
  Conan option `with_sdp=True`, and a pinned Clarabel.cpp build supplied through
  `CUTWIDTH_CLARABEL_ROOT`.
- External PB/SAT requires explicit solver and proof-checker paths. Native
  incremental CaDiCaL is enabled with `CUTWIDTH_CADICAL_ROOT` pointing to the
  pinned, prebuilt source checkout.

Pinned bootstrap helpers and their prerequisites are documented in
`../tools/README.md`. SAT witnesses are checked as layouts, and UNSAT results
are accepted only after independent proof checking.

## Correctness tests

The test suite cross-checks the branch-and-bound engines against subset dynamic
programming and brute-force enumeration on small graphs. It also covers caches,
partial bounds, parallel sessions, checkpoint restoration, memory governance,
PB/SAT integration, and optional SDP components.

`--verify` validates an emitted ordering, but it is not itself an optimality
proof. Only `OPTIMAL` with equal certified bounds proves the optimum.
