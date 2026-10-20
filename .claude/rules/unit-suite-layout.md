---
paths:
  - "tests/unit/**"
  - "Makefile"
  - "scripts/lint-test-registry.py"
---

# The C unit suite's layout

`tests/unit/`: one `test_*.c` per subject, compiled through `tu_*.c`
manifests that `#include` the sources they need plus their `test_*.c`.
`main.c` is only the runner.

- **`#include` a source, don't link it, when a test reaches a `static`** (25
  of 236, e.g. `compare_extents()`, `record_match()`). List it in
  `Makefile`'s `TEST_INLINED` so it is not also linked; a mistake fails the
  link with `multiple definition`.
- **A source goes into exactly one TU**, so tests reaching each other's
  statics share one: progress and search tests (`file_scan`'s work queue,
  `progress.c`'s counters) are `tu_scan`; dbfile loader tests (via
  `find_dupe_extents()`, static in `results-tree.c`) are `tu_dbfile`. Tests
  needing no static go in `tu_plain`, which includes no source.
- The split exists for mutation testing: `mutate.py` reuses lanes, so a mutant
  rebuilds one TU (`dbfile` 0.71 s, `fiemap` 0.41 s, `storage` 0.23 s at
  `-O0`) instead of 1.49 s for all 20,600 lines. A full build is slower
  (~3 s); break-even is two mutants.
- **`minunit.h`'s counters and `proptest.h`'s seed cache are `extern`**,
  defined once in `main.c` beside `blocksize`. As `static`, each TU had its
  own copy: `MU_REPORT()` printed "0 tests" and exited 0, and each TU got its
  own `random` seed.
- **`suite.h` redefines `MU_TEST` for external linkage** so `main.c` owns the
  run order. **Never regroup the `MU_RUN` list**: every `memdb()` handle is
  the same shared-cache in-memory database, so some dbfile tests must run
  before any row is stored.
  - **`scripts/lint-test-registry.py` checks that the set of `MU_TEST` names
    equals the set of `MU_RUN` names** (not the order). As `static`, an
    orphan failed the `WERROR=1` build as unused; with external linkage it
    compiles, links and never runs (a `mu_check(1 == 2)` orphan stayed green).
  - `MU_RUN` declares the test where it runs it, so there is no second list.
    A hand-written declaration header was tried: a missing entry is only an
    implicit-declaration warning, and the suite stayed green.
- **Adding a subject is two edits**: the `test_*.c` and its `#include` in the
  right `tu_*.c` (plus `TEST_INLINED` if it needs a static).
