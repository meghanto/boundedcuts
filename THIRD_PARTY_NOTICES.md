# Third-party notices

BoundedCuts wheels include the following source-pinned command-line tools. Their
complete upstream license texts are installed in `boundedcuts/licenses/`.

- **CaDiCaL 2.1.3**, commit
  `f13d74439a5b5c963ac5b02d05ce93a8098018b8`, Copyright Armin Biere and
  contributors, MIT License, <https://github.com/arminbiere/cadical>.
- **DRAT-trim**, commit
  `2e3b2dc0ecf938addbd779d42877b6ed69d9a985`, Copyright Marijn Heule,
  Nathan Wetzler, and The University of Texas at Austin, MIT License,
  <https://github.com/marijnheule/drat-trim>.

These tools run as separate processes. CaDiCaL generates an UNSAT proof and
DRAT-trim independently verifies the exact CNF/proof pair before BoundedCuts
accepts the result as a certificate.

The Windows DRAT-trim build applies one portability correction: its binary
proof input is opened with C mode `rb` instead of `r`, preventing byte `0x1a`
from being interpreted as end-of-file. Unix builds use the pinned source
unchanged.
