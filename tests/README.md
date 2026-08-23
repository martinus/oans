# oans tests

Two layers:

- **Unit tests** — `unit/`, built and run by `make test`. One file per subject
  (`test_dbfile.c`, `test_fiemap.c`, …) plus `fixtures.h` for the builders more
  than one of them needs. `unit/main.c` is the manifest: it `#include`s the
  sources *and* the test files, so the whole suite stays a single translation
  unit — which is what lets a test reach a `static` function, and what decides
  which files `scripts/mutate/mutate.py` is allowed to sweep.
- **Integration tests** — `integration/`, run by `make integration`. Python
  stdlib `unittest` (no third-party dependencies). They drive the built
  `oans` binary against a scratch directory tree and assert on the
  resulting hashfile (a SQLite database, read with the stdlib `sqlite3` module)
  and on actual on-disk extent sharing (read with the `FIEMAP` ioctl). This
  covers behavior the unit tests can't reach: the scan/dedupe pipeline,
  incremental rescans, rename and hardlink handling, excludes, sparse files, and
  end-to-end data integrity — including the two bugs that motivated the suite
  (the batched-writer hardlink corruption and the `FIDEDUPERANGE` EINVAL
  rejection), both of which only reproduce end to end.

Run both with `make check`.

## Running

```sh
make integration                         # build oans and run the suite
make integration TEST_JOBS=1             # ... sequentially (see -j below)
python3 tests/run.py                      # run directly (binary must be built)
python3 tests/run.py hardlink dedupe      # only tests whose id matches a pattern
python3 tests/run.py -j 8                 # 8 workers ('auto' by default)
python3 -m unittest discover -s tests/integration -v   # plain unittest, no banner
```

The suite runs across worker processes by default, so tests must keep to the
per-test scratch `setUp` hands them. `-j 1` runs them one at a time, which is
where to start if a test only fails in company.

Environment:

- `DUPEREMOVE=/path/to/oans` — test a specific binary (defaults to the one
  in the repo root). Handy for A/B-ing against another build.
- `DUPEREMOVE_TEST_DIR=/mnt/scratch` — where scratch trees are created. **Dedupe
  tests need this to be on a reflink-capable filesystem** (btrfs or xfs); they
  are skipped otherwise. Defaults to `.itest-scratch/` next to the repo.

Exit status is non-zero if any test fails, so it drops straight into CI.

## Layout

```
tests/
  run.py                 runner: banner + substring filtering over unittest
  integration/
    harness.py           DuperemoveTest base class + helpers
    test_*.py            the tests, grouped by theme
```

## Writing a test

Subclass `DuperemoveTest` (from `harness`) and add `test_*` methods. Each test
gets a fresh scratch directory in `self.work` and a per-test hashfile in
`self.hf`; both are cleaned up automatically.

That isolation is what lets the suite run in parallel, so keep to it — a test
reaching outside its own `self.work` will flake. If a test asserts on the
*physical* extent layout (the fsync-forced extent boundary trick, or fiemap
extent counts), set `serial = True` on the class: no amount of file isolation
helps there, because concurrent I/O changes how the kernel lays extents out.

```python
from harness import DuperemoveTest, requires_reflink

@requires_reflink                       # skip the whole class if the fs can't dedupe
class ExampleTest(DuperemoveTest):
    def test_two_copies_get_shared(self):
        a, b = self.mkdup("a", "b", 1 << 20)   # identical, independent copies
        self.assertNotShared(a, b)             # not sharing yet

        self.dedupe(self.path("."))            # scan + dedupe into self.hf
        self.assertDmOk()                      # no error signatures in output
        self.assertShared(a, b)                # kernel reflinked them
```

Key helpers on `DuperemoveTest` (see `harness.py` for the full set):

| Helper | Purpose |
|---|---|
| `scan(path, *extra)` / `dedupe(path, *extra)` | run oans readonly / with `-d` into `self.hf` |
| `dm(*args)` | run with arbitrary args; output in `self.out`, code in `self.rc` |
| `assertDmOk()` | fail if the run printed any error signature (exit code alone is unreliable — oans can exit 0 after per-file failures) |
| `hf_count(table)` / `hf_query(sql)` / `hf_scalar(sql)` | query the hashfile via stdlib `sqlite3` |
| `files_fingerprint()` / `extents_fingerprint()` | stable digest of a table, for equivalence checks |
| `assertShared(a, b)` / `assertNotShared(a, b)` | do two files share physical extents? (via `FIEMAP`) |
| `net_change()` / `assertNoNewSharing()` | the reported "net change in shared extents" |
| `tree_digest(dir)` | content hash of a whole tree, for integrity checks |
| `mkrand` / `mkdup` / `make_sparse` / `write` / `hardlink` | build test data (copies are plain writes, so the pre-dedupe state is genuinely unshared) |
| `requires_reflink` (class decorator) | skip tests that need dedupe when the fs can't |

## The C unit suite

`unit/` is a separate thing from everything above: one translation unit that
`#include`s the other sources, so it can reach their static functions, run by
`make test` in a few hundred milliseconds. One file per subject, with
`main.c` as the manifest and `fixtures.h` for what more than one of them
needs. Two kinds of test live in there.

**Tables of cases**, in minunit's `MU_TEST`, are the majority. They say what a
function is for, and read as documentation of it.

**Properties**, via `src/proptest.h`, state a relationship that must hold for
every input and then generate inputs looking for one where it does not:

```c
MU_TEST(test_prop_sanitize_ctrl_leaves_nothing_dangerous) {
	declare_prop(p, 20000);

	while (prop_next(&p)) {
		char in[24], out[SANITIZE_CTRL_MAX * 24 + 1];

		gen_hostile_name(&p, in, sizeof(in));
		sanitize_ctrl(in, out, sizeof(out));
		prop_check(&p, !has_ctrl(out));
	}
}
```

The seed is fixed so `make test` is reproducible; `OANS_PROPTEST_SEED=random`
goes looking and prints the seed it chose, and a failure names the seed and case
number to replay. `src/proptest.h`'s header comment has the rest, including why
there is no shrinking and what that costs.

Neither kind replaces the other, and the properties do not lift everything
equally. Measured with `scripts/mutate/mutate.py` over `src/util.c`, the same
413 mutants with the properties removed and restored: 213 survivors → 203, all
ten of them in `sanitize_ctrl`'s truncation arithmetic, which needs a buffer
that runs out at each possible offset. Over `src/glob.c` they gained nothing at
all. And writing them for `fiemap_phys_set` found that leaving the address set
unsorted was caught by nothing whatsoever.

## Notes

- The suite is self-validating: running it against a binary with a known bug
  fails the relevant tests. The EINVAL cases fail on a pre-fix build, and the
  hardlink cases fail on a build without the batched-writer seen-inode guard.
- Sharing is detected by reading each file's physical extents with the `FIEMAP`
  ioctl (`harness.phys_extents`) and intersecting them — the same mechanism
  `filefrag` uses, but in-process, so no output parsing.
- On btrfs, `cp` reflinks by default; the builders make duplicate inputs with
  plain writes so they start out with independent storage and dedupe has real
  work to do.
