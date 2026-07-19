# Optional proof backends

The default BoundedCuts build does not need these tools. They bootstrap pinned
external solvers used by optional certified SDP and PB/SAT backends; all build
artifacts are written outside this repository.

Published Python wheels already contain the pinned CaDiCaL and DRAT-trim
executables. These bootstrap scripts are for native/source builds and auditing.

- `bootstrap_pb_sat.sh DESTINATION` fetches and builds pinned Kissat, CaDiCaL,
  and DRAT-trim revisions.
- `bootstrap_pb_sat_windows.ps1 -Destination DIRECTORY` builds the pinned
  CaDiCaL and DRAT-trim proof chain on native Windows with MSYS2 MINGW64. It
  detects common MSYS2 installations and uses `pacman` to install GNU make and
  the MinGW-w64 compiler. The pinned Kissat source is not MinGW-portable and is
  therefore provided only by the Unix helper.
- `bootstrap_clarabel_cpp.sh PREFIX` builds pinned Clarabel.cpp with SDP support
  on macOS or Linux.
- `bootstrap_clarabel_windows.ps1` builds the pinned Clarabel Rust wrapper with
  SDP and a caller-provided OpenBLAS library on Windows.

Read each script before running it. The scripts fetch source code from the
internet and require the upstream projects' build prerequisites.

For the Windows PB/SAT bootstrap, install [Git for Windows](https://git-scm.com/)
and [MSYS2](https://www.msys2.org/), then run from PowerShell:

```powershell
.\tools\bootstrap_pb_sat_windows.ps1 -Destination "$env:LOCALAPPDATA\BoundedCuts\pb-sat"
```

The script prints the pinned solver and proof-checker paths. PB encoding is
implemented inside BoundedCuts; it is not a separate package or benchmark
runner.
