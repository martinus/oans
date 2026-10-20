---
paths:
  - "src/proptest.h"
  - "tests/unit/**"
---

# `src/proptest.h` — property tests

One header, no dependencies, minunit-compatible.

- **The seed is fixed** so `make test` is reproducible.
  `OANS_PROPTEST_SEED=random ./test` picks and prints one; every failure names
  the seed and case number to replay. Each property draws from its own stream
  (seed mixed with the test name), so adding one renumbers no other.
- **There is no shrinking.** Generators produce only small inputs so a
  counterexample is readable as is. A property that needs a large input
  belongs in an ordinary test with a fixture.
- **Three `GRegex` properties are skipped under valgrind**
  (`OANS_PROPTEST_NO_JIT=1`, set by the CI leg; prints what it skipped).
  `G_REGEX_OPTIMIZE` makes PCRE2 JIT code that memcheck reports as
  uninitialised-value errors from symbol-less frames; reproduced with no oans
  code. **Never suppress it instead**: the only possible entry is
  `Memcheck:Cond` over `obj:*`, which hides every uninitialised-value error
  (that check caught the missing memset in `start_running_checksum()`).
- **Keep the suite at ~0.3 s.** The mutation tool's hang timeout scales with a
  green run, so a slow suite turns mutants into `hang` (at 1.2 s two glob
  mutants did). The glob properties try `PROP_GLOB_PATHS` paths per compiled
  pattern, since the compile is nearly all their cost.
- **Never phrase a property only through the function under test.** The
  escaping property's `!has_ctrl(out)` uses `ctrl_seq_len`, as the escaper
  does, so narrowing the C1 test from `>= 0x80` to `> 0x80` went uncaught
  while U+0080 reached the terminal. It now also checks the raw bytes. Add an
  independent second phrasing wherever the obvious property is a tautology.
- **Properties complement the tables; their lift is uneven.**
  - `src/util.c`: 213 → 203 survivors, all in `sanitize_ctrl`'s truncation
    boundary, which tables do not cover.
  - `src/glob.c` (305 mutants): nothing gained.
  - Bug files: only a property catches an unsorted fiemap address set.
  - `longpath.c`: `chunk_end()` was pulled out of `open_ancestor()` as a pure
    function so properties can reach the chunk split without building
    directory trees: 65 → 61, `chunk_end` 4 → 0 (the remaining are the
    deliberate `LONGPATH_MAXLEN + 1`). Killing `<=` → `<` on the limit needed
    a `prop_chance` for a slashless path of exactly `LONGPATH_MAXLEN` (the
    mutant answers `ENAMETOOLONG` for one that fits); components had been
    capped at 300 characters. Read the residue by function to find such
    generator gaps.
  - The three hashfile properties on `src/dbfile.c`: 786 → 785, killing
    `i > 0` → `i > 1` in `dbfile_layout_matches` (a single-extent donor stops
    matching; every table fixture has two records, while btrfs reports any
    contiguous file as one record).
  - `test_prop_a_split_record_is_not_the_layout_it_covers` refuses
    normalizing adjacent records (#186), a design change no operator
    generates. Read `caught` as a lower bound (`mutation-testing.md`).

## Soaking

`OANS_PROPTEST_SCALE=100 ./test` multiplies every property's case count; the
default per-property counts are tuned (the glob ones around `GRegex` compile
cost), so scale them rather than fixing a number.

- **A soak extends a run**: its first N cases are the unscaled run's cases,
  and a failure still names the seed and case to replay.
- Run the wide pass (six random seeds at scale 100: not tuned to the default
  seed) before the deep pass (scale 500), which mostly confirms; more cases
  cannot find a bug in a tautological property.
- Clean at ~1.45 billion assertions over seven runs: no rare input shape is
  missing from the generators. It says nothing about property quality.
