---
paths:
  - "Makefile"
  - ".github/workflows/**"
  - "scripts/lint-build-flags.py"
  - "src/debug.h"
---

# Build flags: WERROR must not drop -O2

- **A variable with the `override` origin ignores every later ordinary
  assignment, wherever it sits.** `ifdef WERROR` did `override CFLAGS +=
  -Werror`, so `CFLAGS += -O2 $(HARDENING)` vanished and every CI job
  (`.github/workflows/ci.yml` sets `WERROR: 1`) tested `-O0`. Hence
  `override CFLAGS += -O2 $(HARDENING)`. (17 of `dbfile.c`'s 1,879 mutants
  differ between `-O0` and `-O2`, see `mutation-testing.md`.)
- `scripts/lint-build-flags.py` compares the `-O` level of `make -Bn` with and
  without `WERROR`, with `WERROR` scrubbed from env and `MAKEFLAGS` for the
  plain run (CI exports it). `-Bn`, not `-n`: on a built tree `make -n`
  prints nothing and passes vacuously. Only `WERROR=1`/`yes`/`true` enable it
  (`ifdef` matched `WERROR=0`); the lint asserts that too.
- **Diagnose from the binary**: `readelf --debug-dump=info ./test | grep
  DW_AT_producer` shows each TU's real flags. Object sizes mislead.
- `src/debug.h` macros `dprintf`/`vprintf` shadow libc names that glibc's
  `bits/stdio2.h` makes macros under `_FORTIFY_SOURCE`, so clang + WERROR +
  `-O2` fails on `-Wmacro-redefined`. They `#undef` first; renaming them is a
  tree-wide change for its own commit.
- **Gate a build-flag change under both compilers**: `CC=clang WERROR=1 make`.
  The local clang build (sanitizer leg) is `-O1` without hardening.
- `-Wformat-truncation` needs optimization; spell a deliberate truncation's
  bound in code, not a comment.
- Optimized, the suite is ~2× faster (soak 221 s → 109 s); ~0.85 s startup is
  fixed.
