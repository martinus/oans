---
paths:
  - "scripts/mutate/**"
  - "tests/unit/**"
  - "tests/lsan.supp"
---

# Mutation & property testing (the C unit suite)

Two additions to the C unit suite's side of the house (`tests/unit/`). Neither touches the
integration suite, and the distinction matters for reading either of them.

## What eight iterations of this actually taught

The findings per subject are in `dedupe-model-tests.md`, `dbfile-tests.md`,
`find-dupes.md`, `fiemap-storage-tests.md` and `proptest.md`. This is the
method they converged on, and it is worth reading before starting a ninth.

- **The loop is: write the tests, review them, sweep, then open the PR — in
  that order.** A review that lands after the sweep invalidates every number in
  it, because a review of tests always edits tests. Sweeping first and
  reviewing second means re-running a sweep that costs minutes to an hour.
- **In seven of seven iterations, the review found a test that could not
  fail.** Not a weak test — a test whose assertion held no matter what the code
  did. The list, because the shapes repeat: a fixture whose winner led on every
  column (so a `min(id)` election passed the #197 test); an `is_anchor`
  assertion that passed against its own mutation, and which turned out to be
  guarding state with no reader at all (#237); two symmetric file layouts,
  which made "which side of the pair was written" unobservable and cost 47 of
  72 mutants; a helper wrapping `list_first_entry()` on a possibly-empty list,
  which never returns NULL, so every `mu_check(t != NULL)` around it held
  whatever the loader did; an idleness check a deleted wait still satisfied; a
  vacuous empty-file scenario; and "a fresh file shares nothing", equally
  satisfied by fiemap mapping nothing at all.
  That rate did not fall as the work got better, which is the argument for the
  review step being mandatory rather than a finishing touch. **Suspect the
  fixture before you suspect the code**, and ask of every new assertion: what
  would have to be true for this to fail?
- **Read every survivor count by function** (`report.py`, below). The number the
  tool prints answers a question nobody asked. `src/util.c` at 213 of 413 was
  5-9% survival wherever a test existed and 85-94% in four untested pure
  functions; the total says one thing and the grouping says two.
- **Check the equivalents rather than counting them.** `storage.c` ended at
  nine survivors in a pure function of which eight are provably equivalent, and
  the discipline is what found the two that were not. Three that *looked*
  equivalent in `src/util.c` were not, and one of those wrote a byte past a
  buffer — so prove it, one at a time, rather than waving at the group.
- **Then stop.** Residue that is an ioctl, a `perror_sqlite` + `goto out` pair,
  or an `exit()` path is triaged, not fixed: the next honest move there is a
  fault-injection seam, and saying so is a better answer than a bigger fixture.
- **A `caught` count is a lower bound on what a test guards, never the
  measure.** Measured four times now: the `src/glob.c` properties gained
  nothing on a sweep, the three hashfile ones moved exactly one mutant (and it
  was worth all three), and the storage property moved *zero* while catching
  its own bug block. No operator generates a design change, so anything a
  bug file guards against is invisible to a sweep by construction.

## `scripts/mutate/mutate.py` — does anything notice when this breaks

Breaks a source file in a throwaway copy of the tree, rebuilds, runs `./test`,
and reports whether anything went red. Coverage says a line ran; this says
something would have noticed it misbehaving.

```sh
scripts/mutate/mutate.py --bugs scripts/mutate/bugs/fiemap.txt
make mutation-replay                                    # every bug file, as CI does
scripts/mutate/mutate.py --file src/glob.c --diff       # only what you changed
scripts/mutate/mutate.py --file src/util.c --dry-run    # how many, and how long
```

- **`--file` is not optional in practice.** oans is not a single-header library,
  so there is no file that is obviously *the* code; the default (`src/csum.c`)
  is a starting point and a run without `--file` is usually the wrong question.
- **`bugs/*.txt` are real oans bugs put back** — #147, #159, #186, #187, #191,
  #202 — one block each, and each file names the source it mutates on its own
  first line (`# file: src/util.c`). That is the everyday mode and the one worth
  adding to: a fix without a block here has no answer to "does its test earn its
  place". A block that stops applying means that part was rewritten and the
  question wants re-deriving, not repairing.
  - **The target belongs in the file, not in its name.** Deriving it from the
    filename works until a bug file is named after a theme rather than a source,
    which `escape.txt` was — and it bought a special case in the CI loop for
    exactly one file. `make mutation-replay` now runs the lot with nothing to
    know, and `make lint` refuses a bug file whose `# file:` line is missing or
    names something that is not there.
  - **One source per bug file, and the lint now enforces that too.** A *second*
    `# file:` line is worse than a missing one, because it does not abort: the
    adapter takes the first match and ignores the rest, so the blocks under the
    later heading are applied to the earlier heading's source. Measured — a
    block written for `src/util.c` (`if (base < 1)`) applied cleanly to
    `src/storage.c`, where the same text exists and the mutant is equivalent,
    and came back **`survived`**, under the report's "nothing noticed these —
    whatever covers them is decoration". A test called decoration when it was
    never run, which is the direction every bug in this tool has erred. This is
    how nine blocks in one file once became three that ran.
- **A `survived` is a narrower claim than it reads.** The suite is `tests/unit/`
  alone: the end-to-end Python suite drives the *binary* and is not in the loop,
  so for the dedupe phase, the scan pipeline and the progress block a survivor
  means nothing at all. The tool says so in its own fingerprint.
- **No sanitizer by default**, so a mutant that reads one slot too far only
  shows up if it corrupts something a test checks. Re-run a surprising survivor
  with `--make-arg SANITIZE=address,undefined --make-arg CC=clang`.
  - **`tests/lsan.supp` was hiding oans's own leaks, and this is how that was
    found.** Its `leak:libglib-2.0` line matches any leak whose stack passes
    through GLib, and oans keeps nearly everything in GLib containers
    (`GPtrArray`, `GHashTable`, `GRegex`, `GString`) — so it suppressed most of
    oans's heap along with the cached-thread residue it was written for.
    Measured: deleting one `g_free()` from `glob_set_free()` leaks 769,200
    bytes in 32,050 allocations, LSan names `glob_set_new()` as the site, and
    the run still exits **0**. Measured over a whole `src/glob.c` sweep, all
    three on one tree: **plain 64 survivors, ASAN with this file 50, ASAN with
    the module-wide pattern 64** — so under the broad file the sanitizer leg was
    worth *nothing at all* over a plain build on this file, and narrowing
    recovers 14, every one a deleted `g_free`/`g_string_free`/`g_regex_unref` or
    an early `return` that skips one. `tests/lsan.supp` now names three
    GLib functions and nothing wider, for **both** legs. A handful of
    deallocation mutants still survive it, on error paths the suite never
    reaches — untested code, not an untested leak.
  - **Narrowing it for the end-to-end leg was measured, not assumed**, which
    took getting around the missing btrfs: force `dedupe_probe_fd()` to answer
    `DEDUPE_SUPPORT_YES` in a throwaway tree (before #232 the same trick was
    stubbing `is_fs_supported()`) and the binary runs its whole scan and dedupe
    pipeline on ext4 — the ioctl fails, but the walkers, the csum pool, the
    dedupe pool, sqlite and the progress block all run. Scan, all three dedupe modes,
    `--io-threads=16`, `--exclude`, `--progress=json`, in-memory, replay,
    `--stats`, `--json`: **zero leaks with no suppressions at all**, and under
    `print_suppressions=1` the three entries never fire. They stay as insurance
    against a GLib that caches more eagerly (measured on 2.80), so one of them
    starting to match is worth looking at rather than assuming.
  - What that could not reach is a *successful* `FIDEDUPERANGE`, which ext4
    refuses — so allocations on the accepted-destination path are unverified,
    and are one CI run on btrfs from being answered.
- **`make test-build` exists for this** and only this: `make test` builds *and
  runs*, so a mutant the tests caught would exit nonzero at the build step and
  be scored `compiler` — the compiler credited with protection the tests
  provided. Don't merge the two targets back together.
- **A file `tests/unit/main.c` does not `#include` is refused, not swept.** `oans.c`
  and `run_dedupe.c` belong to the shipped binary alone, and `make test-build`
  compiles only the manifest - so every mutant in them came back `survived` and
  the report read "whatever covers them is decoration" over 1,394 mutants that
  were never under test. The tool now appends an `#error` to the target and
  builds: if that succeeds, nothing the test binary is made of included the
  file. To sweep either one, put it in the test translation unit first.
- **`mutate_core.py` is vendored** from unordered_dense, where its own hermetic
  suite (`scripts/test_mutate.py`) lives; nanobench holds the same copy. Never
  edit it here — change it there, run that suite, re-copy into all three and
  update each `mutate_core.sha256`. `make lint` fails if this copy has drifted.
  Only `scripts/mutate/mutate.py` (the ~130-line adapter) is oans's.
- **A mutant costs one compile of the `tu_*.c` that includes the mutated
  source** — 0.23-0.71 s at `-O0`, where the old single TU was 1.49 s for every
  mutant whatever was touched. That compile *is* the run: the suite itself is
  0.3 s. The `-fsyntax-only` pre-filter rejects the invalid ones at about a
  tenth of a build, and the adapter's `default_syntax_tu()` maps a source to
  the TU that compiles it — returning the runner instead, as it did briefly
  after the split, compiles a TU the mutation cannot reach, so the filter
  passes every time and quietly stops filtering.
  - **The lanes therefore build at `-O0`** (`MUTANT_CFLAGS` in the adapter),
    which is the single biggest lever there is. Measured, interleaved: the
    makefile's shipping flags (`-O2 -ggdb` plus hardening) compile in 5.6 s
    against 1.3 s, while the suite goes only 0.24 s → 0.30 s — so a mutant is
    **5.84 s → 1.60 s, 3.7×**, and a whole-file sweep of `src/dbfile.c` is 22
    minutes instead of 49. The warning flags stay: dropping them would move
    mutants between `compiler` and the verdicts that are about the tests, which
    is the one comparison this tool exists to make.
  - `--make-arg CFLAGS=...` overrides it (make takes the last assignment on the
    line, and the caller's comes after). A `--make-arg SANITIZE=...` run is
    unaffected either way — the makefile appends its own `-O1 -fsanitize=...`
    through `override CFLAGS +=`, which lands last and wins.
  - **Validated over a whole file, not a sample.** `src/dbfile.c` swept at both
    settings: 908 survivors at `-O2`, 901 at `-O0`, and **17 of 1,879 mutants
    (0.9%) differ**. Every one of the 17 is a *statement deletion* — a removed
    `return ret;` or assignment, which leaves a function falling off the end or
    a value uninitialised. That is undefined behaviour, so its observable
    result depends on codegen and the verdict is not a stable property of the
    tests under any build. Nothing else moved, in either direction.
- **Read a survivor count by function, never as a total.** The first sweep of
  `src/util.c` came back 213 survivors of 413 and that number says nothing:
  grouped by function it was 5-9% survival everywhere a test existed
  (`ctrl_seq_len` 2/37, `sanitize_ctrl` 4/58) and 85-94% in four pure functions
  that had *no test at all* — `parse_size`, `human_size_snprintf`,
  `human_duration_snprintf`, `num_digits`, together 113 of the 213. Writing
  those took it to 94. So the tool measures absent tests and weak tests with
  the same number, and only the grouping tells them apart.
  - `parse_size` is why it mattered: a ladder of `switch` fallthroughs, so one
    missing `mult *= 1024` makes `--max-filesize=10G` mean ten megabytes on a
    run that otherwise looks right. The integration suite passes `1K` and `1M`,
    which left `g`/`t`/`p`/`e` unexercised anywhere.
- **Triage the residue rather than reporting it.** What was left at 94 is
  ~61 mutants in `setrlimit`/`sysconf`/`clock_gettime`/`backtrace` code no unit
  test can reach, 14 on `parse_size`'s `exit()` paths (a test that reached them
  took the suite with it; since #284 it returns an error and they are tested),
  and a handful that are provably equivalent —
  `memcpy(buf, "\\t", 3)` copies the literal's own NUL into a scratch buffer
  whose third byte is never read; `u < ARRAY_SIZE(units) - 1` cannot differ
  from `<=` because `v >= 1024.0` fails first, UINT64_MAX being 16 EiB. Check
  the equivalents rather than assuming them: three that looked equivalent were
  not, and one of those wrote a byte past a buffer.

## `scripts/mutate/report.py` — the grouping, and the diff

`mutate.py --json` writes a report; this reads it. Two modes, and both exist
because they were hand-rolled in throwaway python four times over eight
iterations before earning a file.

```sh
scripts/mutate/report.py sweep.json                 # survivors by function
scripts/mutate/report.py before.json after.json     # what a change moved
```

- **The grouping is the rule above, made cheap.** Kill rate per function is
  computed over the *scored* mutants (total minus `compiler`), since a function
  whose mutants the build all rejected says nothing about the tests.
- **The diff is the claim worth making.** "Survivors went down by four" is the
  same number for a change that killed four and one that killed six and lost
  two, so it names the mutants that went and calls out any that **arrived** —
  a test that stopped covering something is the finding a total cannot show.
  Used to confirm that `storage.c`'s six kills were exactly the six argued to
  be real, and that nothing regressed.
- **It refuses a per-mutant diff across a changed source, and this is the whole
  reason it is careful.** Line numbers move, so mapping one run's lines through
  the other's function map attributes survivors to whatever now sits at that
  number — done by hand once here, it produced a confident report of a
  regression in `extents_search_init`, a function the change had not touched.
  The guard is the mutant population: identical `(line, name)` multisets mean
  the same source, and anything else falls back to per-function counts and says
  so.
- **`scripts/mutate/test_report.py` is hermetic and runs in `make lint`** — no
  compiler, no build, no lanes. It is there because this tool is only ever read
  as the explanation of a number, so a wrong grouping does not fail, it
  persuades: the first parser walked back over a signature into the doc comment
  above it and filed all 36 of `read_rotational`'s mutants under `parent`, a
  word from prose about partitions. A plausible name with a plausible count.
  - The makefile runs the script directly rather than piping it to `tail`.
    `sh` reports a pipeline's *last* command, so a `| tail -1` would return 0
    however the tests went — the check could never fail, which is the exact
    shape of thing this directory exists to catch.
- **Not in `mutate_core.py`, deliberately.** The core is vendored byte-identical
  into unordered_dense and nanobench and tested over there, so a change to it is
  a three-repo errand; this reads only the JSON those runs already emit, and
  nothing about it is oans-specific except the C function parser. Promote it if
  the other two want it, rather than paying that cost up front.
