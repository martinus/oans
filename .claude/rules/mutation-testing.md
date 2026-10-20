---
paths:
  - "scripts/mutate/**"
  - "tests/unit/**"
  - "tests/lsan.supp"
---

# Mutation testing (the C unit suite)

Covers `tests/unit/` only, not the integration suite. Per-subject findings:
`dedupe-model-tests.md`, `dbfile-tests.md`, `find-dupes.md`,
`fiemap-storage-tests.md`, `proptest.md`.

## Method

- **Order: write the tests, review them, sweep, then open the PR.** A review
  edits tests, so a sweep before it must be re-run (minutes to an hour).
- **The review is mandatory: 7 of 7 iterations found a test that could not
  fail.** Ask of every assertion what would make it fail; **suspect the
  fixture before the code**. Shapes seen:
  - a fixture whose winner leads on every column (`min(id)` passed the #197
    test);
  - an assertion on state with no reader (`is_anchor`, #237);
  - symmetric file layouts, so which side was written is unobservable (47 of
    72 mutants);
  - a helper wrapping `list_first_entry()` on a possibly-empty list: never
    NULL, so `mu_check(t != NULL)` always holds;
  - an idleness check a deleted wait still satisfies;
  - a vacuous empty-file scenario;
  - "a fresh file shares nothing", also true when fiemap maps nothing.
- **Read survivor counts by function, never as a total** (`report.py`).
  Absent and weak tests give the same total: `src/util.c` at 213/413 was 5-9%
  survival where tested (`ctrl_seq_len`, `sanitize_ctrl`) and 85-94% in four
  untested pure functions (`parse_size`, `human_size_snprintf`,
  `human_duration_snprintf`, `num_digits`). Testing those: 94.
  - `parse_size` is a `switch` fallthrough ladder: one missing
    `mult *= 1024` makes `--max-filesize=10G` mean 10 MB. The integration
    suite passes only `1K`/`1M`.
- **Prove equivalent mutants one at a time.** Three in `src/util.c` that
  looked equivalent were not; one wrote past a buffer. Proven there:
  `memcpy(buf, "\\t", 3)` (third byte never read);
  `u < ARRAY_SIZE(units) - 1` vs `<=` (`v >= 1024.0` fails first, UINT64_MAX
  is 16 EiB).
- **Then stop.** Triage, don't fix, residue in an ioctl, a `perror_sqlite` +
  `goto out` pair, an `exit()` path, or `setrlimit`/`sysconf`/
  `clock_gettime`/`backtrace` code; the next step there is a fault-injection
  seam, not a bigger fixture. (`parse_size` returns an error since #284, so
  its former `exit()` paths are tested.)
- **A `caught` count is a lower bound on what a test guards, never the
  measure.** No operator generates a design change, so what a bug file guards
  is invisible to a sweep (the `src/glob.c` properties moved 0 mutants, the
  hashfile ones 1, the storage property 0 while catching its bug block).

## `scripts/mutate/mutate.py`

```sh
scripts/mutate/mutate.py --bugs scripts/mutate/bugs/fiemap.txt
make mutation-replay                                    # every bug file, as CI does
scripts/mutate/mutate.py --file src/glob.c --diff       # only what you changed
scripts/mutate/mutate.py --file src/util.c --dry-run    # how many, and how long
```

- **Always pass `--file`**; the default `src/csum.c` is rarely the question.
- **`bugs/*.txt` are real oans bugs put back** (#147, #159, #186, #187, #191,
  #202), one block each. Add a block with every fix. A block that stops
  applying means the code was rewritten: re-derive it, don't repair it.
  - The first line names the target (`# file: src/util.c`), never the
    filename (`escape.txt` needed a CI special case). `make mutation-replay`
    runs every file; `make lint` refuses a missing `# file:` line or one
    naming a nonexistent file.
  - **Exactly one `# file:` line per bug file** (lint-enforced). The adapter
    uses the first and silently applies later blocks to it: a `src/util.c`
    block (`if (base < 1)`) applied to `src/storage.c`, was equivalent there,
    and reported **`survived`**.
- **`survived` is about `tests/unit/` only.** The integration suite is not in
  the loop, so for the dedupe phase, scan pipeline and progress block a
  survivor means nothing.
- **No sanitizer by default.** Re-run a surprising survivor with
  `--make-arg SANITIZE=address,undefined --make-arg CC=clang`.
  - **`tests/lsan.supp` names three GLib functions and nothing wider, for
    both legs.** A `leak:libglib-2.0` line hid every oans leak through GLib
    containers (`GPtrArray`, `GHashTable`, `GRegex`, `GString`; a deleted
    `g_free()` in `glob_set_free()` exited 0). `src/glob.c` survivors: plain
    64, ASAN broad 64, ASAN narrow 50 (deleted `g_free`/`g_string_free`/
    `g_regex_unref` or early `return`s).
  - The end-to-end leg leaks nothing with no suppressions (checked on ext4
    with `dedupe_probe_fd()` forced to `DEDUPE_SUPPORT_YES`; before #232,
    `is_fs_supported()` stubbed) across scan, dedupe modes, `--io-threads=16`,
    `--exclude`, `--progress=json`, in-memory, replay, `--stats`, `--json`.
    Under `print_suppressions=1` the three never fire; they are insurance
    against GLib caching more (measured on 2.80), so investigate a match.
    Unverified: a successful `FIDEDUPERANGE` (ext4 refuses it).
- **Keep `make test-build` separate from `make test`.** `make test` also runs
  the suite, so a caught mutant would be scored `compiler`.
- **A source no `tests/unit/tu_*.c` `#include`s is refused, not
  swept.**
  The tool appends `#error` to it and builds; success means the test binary
  never includes it. `oans.c` and `run_dedupe.c` (1,394 mutants) used to come
  back `survived`. To sweep one, put it in a test TU first.
- **Never edit `mutate_core.py` here.** It is vendored from unordered_dense
  (hermetic suite `scripts/test_mutate.py` there); nanobench has the same
  copy. Change it there, run that suite, re-copy into all three, update each
  `mutate_core.sha256`. `make lint` fails on drift. Only
  `scripts/mutate/mutate.py` (the ~130-line adapter) is oans's.
- **A mutant costs one compile of the `tu_*.c` that includes the source**
  (0.23-0.71 s; the suite runs in 0.3 s). `default_syntax_tu()` must map a
  source to that TU for the `-fsyntax-only` pre-filter; mapping it to the
  runner makes the filter pass everything.
  - **Lanes build at `-O0`** (`MUTANT_CFLAGS`): 5.84 → 1.60 s per mutant vs
    shipping `-O2 -ggdb` plus hardening. Only 17 of `src/dbfile.c`'s 1,879
    verdicts differ, all statement deletions (a removed `return ret;`: UB).
    Keep the warning flags: they decide `compiler` vs a test verdict.
  - `--make-arg CFLAGS=...` overrides it. `--make-arg SANITIZE=...` gets
    `-O1 -fsanitize=...` via the makefile's `override CFLAGS +=` regardless.

## `scripts/mutate/report.py`

Reads `mutate.py --json` output.

```sh
scripts/mutate/report.py sweep.json                 # survivors by function
scripts/mutate/report.py before.json after.json     # what a change moved
```

- Kill rate per function is over scored mutants (total minus `compiler`).
- The diff names the mutants killed and any that **arrived** (a test stopped
  covering something); a change in the total cannot show that.
- **It refuses a per-mutant diff across a changed source.** Line numbers
  move, so survivors land on the wrong function (a hand-made mapping once
  blamed the untouched `extents_search_init`). Same source means identical
  `(line, name)` multisets; otherwise it falls back to per-function counts
  and says so.
- **`scripts/mutate/test_report.py` is hermetic and runs in `make lint`.** A
  wrong grouping persuades instead of failing (an early parser filed all 36
  `read_rotational` mutants under `parent`, a word from a doc comment).
  - The makefile runs it directly. **Never pipe it to `tail`**: `sh` returns
    the last command's status, so the check could never fail.
- **Keep it out of `mutate_core.py`.** Changing the vendored core is a
  three-repo errand; this reads only the JSON, and only its C function parser
  is oans-specific. Promote it if unordered_dense and nanobench want it.
