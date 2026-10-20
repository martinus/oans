---
paths:
  - "Makefile"
  - ".github/workflows/**"
  - "scripts/lint-build-flags.py"
  - "src/debug.h"
---

# WERROR=1 was silently disabling -O2, in every CI job

`ifdef WERROR` does `override CFLAGS += -Werror`, and **once a variable carries
the `override` origin GNU make ignores every later ordinary assignment to it**.
The release block's `CFLAGS += -O2 $(HARDENING)` sat below that, so with WERROR
set it evaporated. No warning, no error - just a different binary.

`.github/workflows/ci.yml` sets `WERROR: 1` for every job, so every check this
project runs - btrfs, xfs, the four valgrind shards, the three sanitizer legs,
the mutation sweep - was verifying `-O0` code while what ships is `-O2`. That is
exactly where UB-sensitive differences hide, and `mutation-testing.md` records that
17 of `dbfile.c`'s 1,879 mutants differ between the two builds.

- **The fix is one word**: `override CFLAGS += -O2 $(HARDENING)`. Order does not
  save you - any later plain assignment loses, wherever it sits.
- **`scripts/lint-build-flags.py` asserts it** rather than a comment claiming
  it, because the original comment was right and the code was wrong for however
  long CI has been green. It asks `make -Bn` what it would run with and without
  `WERROR` and compares the `-O` level. `-Bn` and not `-n`: with the tree built,
  `make -n` prints nothing and a check reading an empty command list passes for
  the wrong reason.
- **Only `WERROR=1`/`yes`/`true` turn it on now**: `ifdef` was true for
  `WERROR=0` too. The lint asserts that as well, and runs its "plain" build
  with `WERROR` scrubbed from the environment and `MAKEFLAGS`, since CI
  exports it to every job and the comparison was otherwise a build against
  itself.
- **Diagnose this from the binary, not the makefile.** `readelf
  --debug-dump=info ./test | grep DW_AT_producer` prints the flags gcc actually
  compiled with, grouped - which is how the mixed build was found (19 TUs at
  `-O0`, one at `-O2` from a stray plain `make`). An object-size comparison sent
  me the wrong way twice; the producer string cannot.
- **It also surfaced a name collision that had been latent for years.**
  `src/debug.h` defines macros called `dprintf` and `vprintf`, which are also
  POSIX and C89 functions - and glibc turns both into macros in
  `bits/stdio2.h` *only when `_FORTIFY_SOURCE` is on*, which needs
  optimization. clang then reports `-Wmacro-redefined` and `-Werror` stops the
  build; gcc does not. So the combination that fails is clang + WERROR + `-O2`,
  which is exactly the build nobody had ever run. `#undef` first, and note that
  shadowing two libc names is still a trap for whoever wants the real ones -
  renaming them is a tree-wide change for its own commit.
- **Gate a build-flag change under *both* compilers.** The sanitizer leg is the
  only clang build in the local gate and it uses `-O1` with no hardening (the
  makefile says why), so clang + `-O2` + FORTIFY was unbuilt locally and went
  red in CI. `CC=clang WERROR=1 make` is one command.
- **Fixing it immediately surfaced two `-Wformat-truncation` warnings CI had
  never seen**, because that analysis needs optimization to run at all. Both
  were deliberate truncations whose intent lived in a comment; they now spell
  the bound out in code the compiler can follow.
- **The suite is ~2x faster optimized** - measured on the property soak, 221 s →
  109 s for an identical 659,961,110 assertions. The fixed ~0.85 s of startup
  does not optimize, so small runs show less.
