# Third-party notices

BoundedCuts wheels include code from the following source-pinned tools. Their
complete upstream license texts are installed in `boundedcuts/licenses/`.

- **CaDiCaL 2.1.3**, commit
  `f13d74439a5b5c963ac5b02d05ce93a8098018b8`, Copyright Armin Biere and
  contributors, MIT License, <https://github.com/arminbiere/cadical>.
- **DRAT-trim**, commit
  `2e3b2dc0ecf938addbd779d42877b6ed69d9a985`, Copyright Marijn Heule,
  Nathan Wetzler, and The University of Texas at Austin, MIT License,
  <https://github.com/marijnheule/drat-trim>. The license for the committed
  adaptation is retained in `LICENSES/DRAT-trim.txt`.

The default backend compiles both projects with the wheel's native compiler.
CaDiCaL generates binary DRAT bytes in memory and the independently adapted
DRAT-trim algorithm verifies the exact CNF/proof pair before BoundedCuts accepts
the result as a certificate. The adapted checker replaces file parsing with a
portable byte-span reader and converts process exits into fail-closed results.

Unix wheels additionally carry the original command-line programs for explicit
external differential testing. Windows wheels omit those programs and build the
embedded path with MSVC; no MinGW or MSYS runtime enters the wheel.
