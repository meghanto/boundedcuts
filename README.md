<p align="center">
  <img src="assets/boundedcut.png" alt="BoundedCuts logo" width="220">
</p>

<h1 align="center">BoundedCuts</h1>

BoundedCuts is a modern, certifying C++20 cutwidth solver. A
completed exact run reports matching lower and upper bounds; interrupted runs
report only the interval and a verified feasible ordering.

## Repository map

- `exact_solver/` — solver source, CMake project, and correctness tests.
- `examples/` — small inputs for CLI smoke tests.
- `tools/` — pinned bootstrap helpers for optional proof backends.

## Python and NumPy

Released versions provide native wheels for supported Windows, macOS, and
Linux systems:

```sh
pip install boundedcuts
```

The macOS wheels target macOS 11 or newer.

Installing a compatible wheel only unpacks its native extension, bundled CLI,
and proof tools; it does not compile locally. A source installation requires a
C++20 toolchain and lets the build backend restore pinned Boost and Dispenso
packages with Conan. It also builds the pinned proof components; native Windows
source builds require Git for Windows and an MSVC C/C++ toolchain:

```sh
pip install .
```

```python
import numpy as np
import boundedcuts

edges = np.array([[0, 1], [1, 2], [2, 0]], dtype=np.uint32)
graph = boundedcuts.from_edges(edges, num_vertices=3)
result = boundedcuts.solve(
    graph,
    threads=2,
    controller="adaptive",
    verify=True,
)

assert result.optimal and result.lower_bound == result.upper_bound == 2
print(result.ordering)  # owned, C-contiguous numpy.uint32 array
```

A C-contiguous `uint32[m, 2]` array is scanned directly without constructing
Python edge objects or an intermediate C++ edge list. The graph then owns its
normalized adjacency representation, and the native solve releases the Python
GIL. The package and its wheels are licensed under GPL-3.0-only.

Every wheel compiles pinned CaDiCaL 2.1.3 and an independently adapted
DRAT-trim checker into the native extension. The default `pb-sat-root` backend
runs both in-process and carries the proof as binary DRAT bytes in memory;
CaDiCaL's UNSAT result never changes a certified bound until DRAT-trim checks
the exact CNF/proof pair. Unix wheels also retain the pinned command-line tools
as an explicit `external` differential backend. Windows wheels stay entirely
within the MSVC toolchain and omit that executable fallback. Use
`boundedcuts.capabilities()` and `boundedcuts.proof_tool_provenance()` to inspect
the installed backends and exact upstream revisions.

## Portable build

The supported default build uses CMake 3.20+, a C++20 compiler, Python 3, and
Conan 2. Conan restores the required Boost and Dispenso dependencies. The
default binary includes combinatorial partial bounds, residual DP, Lagrangian
bounds, the SDP formulation, and exact certificate verification. External
numerical solvers and proof-producing SAT backends remain optional.

Install Conan, create a profile, and restore dependencies:

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

On Windows, in PowerShell or a Developer Command Prompt:

```powershell
cmake --preset conan-default -DCUTWIDTH_ENABLE_HIGHS=OFF
cmake --build --preset conan-release --parallel
ctest --test-dir build -C Release --output-on-failure
```

Conan generates `exact_solver/CMakeUserPresets.json` and the referenced build
presets for the detected platform. The file is local build state and is ignored
by Git.

## Smoke test

From `exact_solver/` after building, run:

```sh
./build/Release/cutwidth_exact ../examples/triangle.edgelist --json --verify
```

On Windows, use `build\Release\cutwidth_exact.exe`. The JSON result must report
`"status":"OPTIMAL"`, `"lower_bound":2`, `"upper_bound":2`, and
`"verified":true`.

See `exact_solver/README.md` for CLI usage and optional certified proof
backends.

## Reproducibility and contributions

The default build and tests are exercised on Windows, macOS, and Linux in
GitHub Actions. Please keep solver claims tied to verified bounds and include a
small regression test for behavior changes.

## Methods and references

BoundedCuts combines established exact-search, bounding, heuristic, conic, and
proof-producing SAT methods. These citations identify the closest published
lineage for code that is present in this repository; they do not claim that the
implementation reproduces every algorithm in each paper.

- **Cutwidth and exact layout search:** Díaz, Petit, and Serna,
  [*A survey of graph layout problems*](https://doi.org/10.1145/568522.568523);
  Martí et al., [*Branch and bound for the cutwidth minimization problem*](https://doi.org/10.1016/j.cor.2012.05.016);
  and Bodlaender et al., [*A note on exact algorithms for vertex ordering problems on graphs*](https://doi.org/10.1007/s00224-011-9312-0).
- **Search ordering, symmetry, and parallel branch-and-bound:** Haralick and
  Elliott, [*Increasing tree search efficiency for constraint satisfaction problems*](https://doi.org/10.1016/0004-3702(80)90051-X);
  Gent and Smith, [*Symmetry breaking in constraint programming*](https://www.dcs.st-and.ac.uk/~ianm/CSPLib/prob/prob019/gent-smith-2000.pdf);
  and Gendron and Crainic, [*Parallel branch-and-bound algorithms: Survey and synthesis*](https://doi.org/10.1287/opre.42.6.1042).
- **Combinatorial cutwidth bounds:** Kloeckner,
  [*Cutwidth and degeneracy of graphs*](https://arxiv.org/abs/0907.5138), and
  Bermond et al., [*New lower bounds on the cutwidth of graphs*](https://doi.org/10.1016/j.cor.2025.107130),
  together with the cutwidth-specific bounds of Martí et al. above.
- **Annealing and graph-layout local search:** Kirkpatrick, Gelatt, and Vecchi,
  [*Optimization by simulated annealing*](https://doi.org/10.1126/science.220.4598.671);
  Johnson et al., [*Optimization by simulated annealing: An experimental evaluation; Part I, graph partitioning*](https://doi.org/10.1287/opre.37.6.865);
  Mladenović and Hansen, [*Variable neighborhood search*](https://doi.org/10.1016/S0305-0548(97)00031-2);
  Duarte et al., [*Parallel variable neighbourhood search strategies for the cutwidth minimization problem*](https://doi.org/10.1093/imaman/dpt026);
  and Santos and de Carvalho, [*Tailored heuristics in adaptive large neighborhood search applied to the cutwidth minimization problem*](https://doi.org/10.1016/j.ejor.2019.07.013).
- **Spectral orderings:** Fiedler,
  [*Algebraic connectivity of graphs*](https://doi.org/10.21136/CMJ.1973.101168),
  and Atkins, Boman, and Hendrickson,
  [*A spectral algorithm for seriation and the consecutive ones problem*](https://doi.org/10.1137/S0097539795285771).
- **Semidefinite graph-partition and cutwidth bounds:** Wolkowicz and Zhao,
  [*Semidefinite programming relaxations for the graph partitioning problem*](https://doi.org/10.1016/S0166-218X(99)00102-X);
  Gaar, Puges, and Wiegele,
  [*Strong SDP based bounds on the cutwidth of a graph*](https://doi.org/10.1016/j.cor.2023.106449);
  and Goulart and Chen,
  [*Clarabel: An interior-point solver for conic programs with quadratic objectives*](https://doi.org/10.1007/s12532-026-00320-7).
- **Exact certification of numerical bounds:** Jansson,
  [*Rigorous lower and upper bounds in linear programming*](https://doi.org/10.1137/S1052623402416839);
  Gershgorin, *Über die Abgrenzung der Eigenwerte einer Matrix* (1931); and
  Bareiss, [*Sylvester's identity and multistep integer-preserving Gaussian elimination*](https://doi.org/10.1090/S0025-5718-1968-0226829-0).
- **Subset dynamic programming and reusable layout state:** Zündorf,
  [*Minimum Linear Arrangement Revisited*](https://scale.iti.kit.edu/_media/resources/theses/ma_zuendorf.pdf),
  and Cavero et al., [*Multistart search for the cyclic cutwidth minimization problem*](https://doi.org/10.1016/j.cor.2020.105116),
  together with Bodlaender et al. above.
- **Pseudo-Boolean cardinality encodings and checked SAT proofs:** Sinz,
  [*Towards an optimal CNF encoding of Boolean cardinality constraints*](https://doi.org/10.1007/11564751_73);
  Bailleux and Boufkhad, [*Efficient CNF encoding of Boolean cardinality constraints*](https://doi.org/10.1007/978-3-540-45193-8_8);
  Biere et al., [*CaDiCaL, Kissat, Paracooba, Plingeling and Treengeling entering the SAT Competition 2020*](https://cca.informatik.uni-freiburg.de/papers/BiereFleuryHeisinger-SAT-Competition-2020-solvers.pdf);
  and Wetzler, Heule, and Hunt,
  [*DRAT-trim: Efficient checking and trimming using expressive clausal proofs*](https://doi.org/10.1007/978-3-319-09284-3_31).

## License

Original software and documentation in this repository are licensed under
GPL-3.0-or-later. See `LICENSE` for the complete terms.
