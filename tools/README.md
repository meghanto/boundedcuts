# Optional proof backends

The default BoundedCuts build does not need these tools. They bootstrap pinned
external solvers used by optional certified SDP and PB/SAT backends; all build
artifacts are written outside this repository.

Published Python wheels use embedded CaDiCaL and DRAT-trim by default. These
bootstrap scripts are for native/source builds and auditing.

- `bootstrap_pb_sat.sh DESTINATION` fetches and builds pinned Kissat, CaDiCaL,
  and DRAT-trim revisions.
- `bootstrap_pb_sat_windows.ps1 -Destination DIRECTORY` fetches the pinned
  CaDiCaL and DRAT-trim source trees. CMake then compiles the embedded backend
  with MSVC as part of the normal Windows build.
- `bootstrap_clarabel_cpp.sh PREFIX` builds pinned Clarabel.cpp with SDP support
  on macOS or Linux.
- `bootstrap_clarabel_windows.ps1` builds the pinned Clarabel Rust wrapper with
  SDP and a caller-provided OpenBLAS library on Windows.

Read each script before running it. The scripts fetch source code from the
internet and require the upstream projects' build prerequisites.

For the Windows PB/SAT bootstrap, install
[Git for Windows](https://git-scm.com/), then run from PowerShell:

```powershell
$pb = .\tools\bootstrap_pb_sat_windows.ps1 `
  -Destination "$env:LOCALAPPDATA\BoundedCuts\pb-sat"
cd exact_solver
cmake --preset conan-default `
  "-DCUTWIDTH_CADICAL_ROOT=$($pb.CaDiCaLSource)" `
  "-DCUTWIDTH_DRAT_TRIM_ROOT=$($pb.DratTrimSource)"
cmake --build --preset conan-release --parallel
```

PB encoding is implemented inside BoundedCuts; it is not a separate package or
benchmark runner.
