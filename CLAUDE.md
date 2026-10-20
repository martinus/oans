# oans — working notes for Claude

`oans` (a fork of duperemove) finds duplicate extents and deduplicates them via
the kernel `FIDEDUPERANGE` ioctl (atomic, byte-verified). Hashes live in a
SQLite **hashfile** (WAL mode, `synchronous=OFF`, `cache_size=-65536` = 64 MB).

C sources are under `src/` (main `src/oans.c`); the C unit suite under
`tests/unit/`, one file per subject behind a single-translation-unit manifest;
man-page sources under `docs/man/`. The binary is `oans`; `make install` adds a `duperemove` compat
symlink. Some identifiers keep the old name on purpose: the `DUPEREMOVE*` env
vars and the `DuperemoveTest` python base class.

**Local dev box:** `source scripts/devenv.sh` once per shell (see its header) so
`make`, `scripts/verify.sh`, etc. need no env prefixes. This box can't `dnf
install` the `-devel` packages, hence the `/tmp/devroot` pkg-config shim; a
normal machine with the README's deps needs none of it.

## Subsystem notes: `.claude/rules/`

Each subsystem's rules, measurements and past bugs are in one file under
`.claude/rules/`. A file loads by itself when you read or edit a path in its
`paths:` list. Open it yourself when you work on its subject before you have
opened one of those paths: when you plan, reproduce an issue or answer a
question. **Before optimizing anything, read `performance.md` and
`dead-ends.md`.**

| File | Subject |
| --- | --- |
| `scan-walk.md` | walker threads, the single consumer, `statx` (#278) |
| `fs-probe.md` | which filesystems a scan accepts, the `FIDEDUPERANGE` probe (#224) |
| `roots-and-paths.md` | one filesystem per run, `absolute_path()` (#282) |
| `exclude-glob.md` | `--exclude` (`.gitignore` syntax, #147, #283) |
| `readonly-subvols.md` | read-only subvolumes are scanned by default (#182) |
| `untrusted-names.md` | printing file names: `path_for_display()` (#202) |
| `hash-resume.md` | checkpoints for very large files (#159) |
| `prealloc-holes.md` | preallocated extents and holes in digests (#273) |
| `layout-copy.md` | snapshot-aware scan, copying hashes (#206, #287) |
| `interrupt.md` | SIGINT/SIGTERM flush the batch (#201) |
| `hashfile-sqlite.md` | WAL, the scan writer, temp B-trees, the pass loaders' SQL |
| `hashfile-schema.md` | `dbfile_identify()`, when to bump `DB_FILE_MINOR` |
| `hashfile-config-history.md` | replay, `run_history`, report modes, systemd |
| `dedupe-seq.md` | generations and the target election across windows |
| `dedupe-pipeline.md` | the streaming dedupe phase, filerec lifetimes |
| `dedupe-convergence.md` | a second run must reclaim 0 (#186, #280) |
| `correctness-invariants.md` | Ctrl+C safety, what "reclaimed" means (#187, #191) |
| `dedupe-progress.md` | byte-weighted dedupe progress |
| `live-block.md` | output while the progress block is on screen (#179, #286) |
| `io-threads.md` | the `--io-threads` default from the storage type |
| `find-dupes.md` | partial-mode extent search |
| `build-flags.md` | `WERROR=1` and `-O2`, clang + FORTIFY |
| `test-hooks.md` | adding a `DUPEREMOVE_*` test hook |
| `integration-tests.md` | building extent layouts in integration tests (#242) |
| `unit-suite-layout.md` | the C unit suite's translation units |
| `mutation-testing.md` | `mutate.py`, `report.py`, bug files, how to read survivors |
| `proptest.md` | property tests and soaking them |
| `dedupe-model-tests.md`, `dbfile-tests.md`, `fiemap-storage-tests.md` | unit-test findings per subject |
| `valgrind.md` | memcheck runs and suppressions |
| `performance.md` | `bench.py`, `bench-dedupe.py`, `bench-queries.py`, perf |
| `dead-ends.md` | optimizations that were measured and rejected |
| `man-page.md` | `make doc`, pandoc |

**A lesson goes into the file for its subject**, next to the bullet that
already holds that kind of bug (grep `.claude/rules/` for the symbol first). A
new subject gets a new file with a `paths:` list and a row in this table.
Write each rule once: a rule in two files drifts.

## Repo layout & workflow

- **Branch from `origin/master`**: `git fetch origin && git checkout -b
  <branch> origin/master`. A local `master` may be stale or checked out in
  another worktree. Park a worktree afterwards with `git checkout --detach
  origin/master`.
- **GitHub is a fork:** every `gh` command needs `--repo martinus/oans`.
- **A PR description closes every issue it names after a closing keyword.**
  "Also fixes #284 item 1" closed all of #284 on merge. For part of an issue,
  write "part of #N" or "see #N"; keep "Fixes" for the issues the PR finishes.
- **Never merge a PR without the user explicitly saying "merge it".** Rhythm:
  branch → PR → wait. The user often asks for a `/simplify` pass first. Merge
  with `--squash`: the local permission classifier blocks `gh pr merge --merge`.
  - **The one exception is the issues loop.** "Do the issues" (or `/issues`)
    starts `.claude/skills/issues/SKILL.md`, and that request is the
    permission to merge the loop's own PRs, each one only when CI is green.
    It ends when the loop ends; nothing else is relaxed. The skill is the
    order of the work, this file and `.claude/rules/` are the rules, so a
    rule goes there and not into the skill.
- **No Claude attribution anywhere.** Do not add a `Co-Authored-By` or
  `Claude-Session` trailer to a commit. Do not add a "Generated with Claude
  Code" line or a session link to a PR, an issue, a comment, a review or
  release notes. `.claude/settings.json` turns off the trailers and PR lines
  that Claude Code adds itself. This rule covers the text you write yourself.
  - **The GitHub server adds a footer by itself.** A PR description or a
    comment posted through the GitHub tools gets a "_Generated by Claude
    Code_" line, even when you did not write one. Read the text back after you
    post it, and edit the line out: an edit does not add it again (seen on
    #266).
- **`scripts/verify.sh`** is the pre-PR gate: build (warnings = failure),
  `make check`, and a valgrind scan+dedupe+replay smoke.
- **Every path oans prints goes through `path_for_display()`**: file names are
  chosen by whoever can write to the scanned tree. `make lint` enforces it;
  `untrusted-names.md` has the idiom.
- Confirm you're testing *this* `./oans`, not a system `duperemove`, before
  diagnosing runtime behaviour.
- Releases: the `release` skill (`.claude/skills/release/SKILL.md`).

## Build & test

```sh
make -j$(nproc)                        # build oans
make check                             # C unit tests + Python integration suite
DUPEREMOVE=./oans python3 tests/run.py # integration suite only
```

Integration tests are stdlib `unittest` (no deps); they drive the built binary
against a scratch tree and assert on the hashfile and on-disk sharing. Dedupe
cases need a reflink fs (`DUPEREMOVE_TEST_DIR`, set by `devenv.sh`). Keep tests
in `tests/`; no shell tests.

- **The suite runs in parallel by default** (`make integration TEST_JOBS=…`,
  `tests/run.py -j`; the why is in that file's docstring). 26.4→4.3s on 4 cores;
  the real payoff is the sanitizer builds, which re-run all of it. **A new test
  must not share state outside `setUp`'s per-test `mkdtemp` + hashfile** or it
  will flake in parallel; `TEST_JOBS=1` is the sequential fallback for pinning
  such a flake down. The suite is I/O-bound, so `auto` deliberately
  over-subscribes (`2 × nproc`, capped) — don't "fix" it back to `nproc`.
- **A test asserting on the *physical* extent layout must set `serial = True`**
  (`DuperemoveTest.serial`), which holds it back to a one-at-a-time pass after
  the pool drains. Per-test scratch isolation doesn't help here: the physical
  layout and fiemap counts depend on btrfs writeback, which concurrent I/O
  perturbs — CI caught exactly this on btrfs
  (`test_extent_order_independent`, `test_streaming_dedupe`) while xfs passed.
  The four affected files are already marked.
- **Never scan/benchmark out of `/tmp` — it's tmpfs**, not reflink-capable and
  rejected by `is_fs_supported()`, so a scan there stores **0 files silently**
  and dedupe is a no-op. Use real btrfs/xfs and verify a non-zero file count
  before trusting any before/after. (The `~/git` tree, ~174k files on btrfs, is
  the usual benchmark target.)
