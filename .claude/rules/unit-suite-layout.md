---
paths:
  - "tests/unit/**"
  - "Makefile"
  - "scripts/lint-test-registry.py"
---

# The C unit suite's layout

`tests/unit/`, one file per subject, compiled as **one translation unit per
subject** — the `tu_*.c` files are thin manifests that `#include` the sources
they need plus the `test_*.c` beside them. `main.c` is the runner and nothing
else.

- **Sources are `#include`d, not linked, wherever a test reaches a `static`.**
  25 of the 236 statics are called from tests — among them `compare_extents()`
  and `record_match()`, where writing the tests found a real bug in
  partial-mode dedupe. `Makefile`'s `TEST_INLINED` names those sources so they
  are *not* also linked; get it wrong and the link says `multiple definition`
  naming the line, so it cannot drift silently.
- **The TU boundaries are set by that coupling, not by subject.** A source can
  be `#include`d into exactly one TU, so tests that reach into each other's
  statics must share one: a progress test drives `file_scan`'s work queue and
  the search test asserts on `progress.c`'s counters, hence `tu_scan`; a dbfile
  loader test asserts through `find_dupe_extents()`, static in
  `results-tree.c`, hence `tu_dbfile`. Everything that needs no static is
  `tu_plain`, which includes no source at all.
- **Why it is worth the shape.** A mutant rebuilds one subject instead of all
  20,600 lines, because `mutate.py` reuses a lane across mutants and make can
  then build incrementally. Measured at the lanes' `-O0`: `dbfile` 0.71 s,
  `fiemap` 0.41 s, `storage` 0.23 s, against **1.49 s for every mutant** under
  the old single TU. The *full* build got slower (1.49 → ~3 s), since headers
  and fixtures are parsed once per TU — so it pays only because lanes are
  reused, break-even at two mutants.
- **Two vendored headers are forked, and both would have failed silently.**
  `minunit.h`'s counters and `proptest.h`'s seed cache were `static`, which is
  right for one TU: per-TU copies would make `MU_REPORT()` print the runner's
  "0 tests" and exit 0, and would give each TU its own `random` seed. Both are
  plain `extern` now, defined once in `main.c` beside `blocksize`.
- **`MU_TEST` is redefined in `suite.h` to give tests external linkage**, so
  `main.c` can keep the run order while the bodies live elsewhere. That order
  is load-bearing: every `memdb()` handle opens the same shared-cache in-memory
  database, so some dbfile tests must run before anything has stored a row.
  Never regroup the `MU_RUN` list to match the file layout.
  - **That linkage removed a guarantee, and `scripts/lint-test-registry.py` is
    what puts it back.** While tests were `static`, one nobody ran was an
    unused function and `WERROR=1` failed the build naming it. Now an orphan
    compiles clean, links clean, and never runs — measured, with a body of
    `mu_check(1 == 2)`, the suite still reported 111 tests and 0 failures. The
    lint compares the *set* of `MU_TEST` names against the set of `MU_RUN`
    names, and never the order.
  - `MU_RUN` declares the test at the point it runs it, so there is no second
    list. A hand-written header of 111 declarations was tried first: leaving a
    test out of it is only an implicit-declaration *warning*, so a default
    build succeeded and the suite still went green.
- **Adding a subject is two edits**: the `test_*.c`, and its `#include` in the
  right `tu_*.c` (plus `TEST_INLINED` if it needs a static).
