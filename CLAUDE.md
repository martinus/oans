# oans

- `oans` (a duperemove fork) finds duplicate extents and dedupes them via the
  `FIDEDUPERANGE` ioctl (atomic, byte-verified). Hashes live in a SQLite
  **hashfile** (WAL, `synchronous=OFF`, `cache_size=-65536` = 64 MB).
- C sources: `src/` (main `src/oans.c`). C unit suite: `tests/unit/`, one file
  per subject behind a single-translation-unit manifest. Man-page sources:
  `docs/man/`.
- `make install` adds a `duperemove` compat symlink. The `DUPEREMOVE*` env vars
  and the `DuperemoveTest` python base class keep the old name on purpose.
- Dev box: `source scripts/devenv.sh` once per shell, so `make`,
  `scripts/verify.sh` etc. need no env prefixes. The `/tmp/devroot` pkg-config
  shim exists because this box cannot `dnf install` the `-devel` packages; a
  machine with the README's deps needs none of it.

## Subsystem rules: `.claude/rules/`

A rule file loads by itself when you read or edit a path in its `paths:` list.
Open it yourself when working on its subject before touching such a path
(planning, reproducing, answering). **Before optimizing, read `performance.md`
and `dead-ends.md`.**

| File | Subject |
| --- | --- |
| `scan-walk.md` | walker threads, single consumer, `statx` (#278) |
| `fs-probe.md` | accepted filesystems, the `FIDEDUPERANGE` probe (#224) |
| `roots-and-paths.md` | one filesystem per run, `absolute_path()` (#282) |
| `exclude-glob.md` | `--exclude`, `.gitignore` syntax (#147, #283) |
| `readonly-subvols.md` | read-only subvolumes scanned by default (#182) |
| `untrusted-names.md` | printing file names, `path_for_display()` (#202) |
| `hash-resume.md` | checkpoints for very large files (#159) |
| `prealloc-holes.md` | preallocated extents, holes in digests (#273) |
| `layout-copy.md` | snapshot-aware scan, copying hashes (#206, #287) |
| `interrupt.md` | SIGINT/SIGTERM flush the batch (#201) |
| `hashfile-sqlite.md` | WAL, scan writer, temp B-trees, pass-loader SQL |
| `hashfile-schema.md` | `dbfile_identify()`, when to bump `DB_FILE_MINOR` |
| `hashfile-config-history.md` | replay, `run_history`, report modes, systemd |
| `dedupe-seq.md` | generations, target election across windows |
| `dedupe-pipeline.md` | streaming dedupe phase, filerec lifetimes |
| `dedupe-convergence.md` | a second run must reclaim 0 (#186, #280) |
| `correctness-invariants.md` | Ctrl+C safety, "reclaimed" (#187, #191) |
| `dedupe-progress.md` | byte-weighted dedupe progress |
| `live-block.md` | output while the progress block is up (#179, #286) |
| `io-threads.md` | `--io-threads` default from storage type |
| `find-dupes.md` | partial-mode extent search |
| `build-flags.md` | `WERROR=1` and `-O2`, clang + FORTIFY |
| `test-hooks.md` | adding a `DUPEREMOVE_*` test hook |
| `integration-tests.md` | extent layouts in integration tests (#242) |
| `unit-suite-layout.md` | C unit suite translation units |
| `mutation-testing.md` | `mutate.py`, `report.py`, bug files, survivors |
| `proptest.md` | property tests, soaking |
| `dedupe-model-tests.md` | unit-test findings: dedupe model |
| `dbfile-tests.md` | unit-test findings: hashfile |
| `fiemap-storage-tests.md` | unit-test findings: fiemap, storage |
| `valgrind.md` | memcheck runs, suppressions |
| `performance.md` | `bench.py`, `bench-dedupe.py`, `bench-queries.py`, perf |
| `dead-ends.md` | measured and rejected optimizations |
| `man-page.md` | `make doc`, pandoc |

- **Put a lesson in the file for its subject**, next to the bullet holding
  that kind of bug; grep `.claude/rules/` for the symbol first. A new subject
  gets a new file with a `paths:` list and a row in this table.
- Write each rule once: a rule in two files drifts.

## Workflow

- **Branch from `origin/master`**: `git fetch origin && git checkout -b
  <branch> origin/master`. Local `master` may be stale or held by another
  worktree. Park a worktree afterwards with `git checkout --detach
  origin/master`.
- Every `gh` command needs `--repo martinus/oans` (GitHub fork).
- **A closing keyword closes every issue it names** ("Also fixes #284 item 1"
  closed all of #284). For part of an issue write "part of #N" or "see #N";
  "Fixes" only for issues the PR finishes.
- **Never merge a PR unless the user says "merge it".** Rhythm: branch → PR →
  wait; the user often asks for `/simplify` first. Merge with `--squash`: the
  local permission classifier blocks `gh pr merge --merge`.
  - **Exception: the issues loop.** "Do the issues" (or `/issues`) starts
    `.claude/skills/issues/SKILL.md` and permits merging the loop's own PRs,
    each only when CI is green. It ends with the loop; nothing else is
    relaxed. The skill is the order of work; rules go in this file and
    `.claude/rules/`, never the skill.
- **No Claude attribution anywhere.** No `Co-Authored-By` or `Claude-Session`
  commit trailer; no "Generated with Claude Code" line or session link in a
  PR, issue, comment, review or release notes. `.claude/settings.json` turns
  off what Claude Code adds itself; this rule covers text you write.
  - **The GitHub server adds a "_Generated by Claude Code_" footer** to PR
    descriptions and comments posted through the GitHub tools. Read the text
    back and edit the line out; an edit does not re-add it (seen on #266).
- **`scripts/verify.sh` is the pre-PR gate**: build (warnings = failure),
  `make check`, valgrind scan+dedupe+replay smoke.
- **Every path oans prints goes through `path_for_display()`**: file names are
  chosen by whoever writes to the scanned tree. `make lint` enforces it;
  idiom in `untrusted-names.md`.
- Confirm you are running *this* `./oans`, not a system `duperemove`, before
  diagnosing runtime behaviour.
- Releases: the `release` skill (`.claude/skills/release/SKILL.md`).

## Build & test

```sh
make -j$(nproc)                        # build oans
make check                             # C unit tests + Python integration suite
DUPEREMOVE=./oans python3 tests/run.py # integration suite only
```

- Integration tests: stdlib `unittest`, drive the built binary on a scratch
  tree, assert on the hashfile and on-disk sharing. Dedupe cases need a
  reflink fs (`DUPEREMOVE_TEST_DIR`, set by `devenv.sh`). Tests live in
  `tests/`; no shell tests.
- **The suite runs in parallel** (`make integration TEST_JOBS=…`,
  `tests/run.py -j`; reasons in its docstring; 26.4→4.3 s on 4 cores).
  **A new test must not share state outside `setUp`'s per-test `mkdtemp` +
  hashfile** or it flakes. `TEST_JOBS=1` runs sequentially to pin a flake.
  `auto` over-subscribes on purpose (`2 × nproc`, capped; the suite is
  I/O-bound): don't change it to `nproc`.
- **A test asserting on the *physical* extent layout sets `serial = True`**
  (`DuperemoveTest.serial`: runs one at a time after the pool drains).
  Per-test scratch does not help: btrfs writeback under concurrent I/O
  changes the layout and fiemap counts (`test_extent_order_independent`,
  `test_streaming_dedupe` failed on btrfs CI).
- **Never scan or benchmark in `/tmp`: it is tmpfs**, which has no
  `FIDEDUPERANGE`, so the scan refuses it (`fs-probe.md`). Use btrfs/xfs and
  check for a non-zero file count before trusting a before/after. Usual
  benchmark target: `~/git` (~174k files, btrfs).
