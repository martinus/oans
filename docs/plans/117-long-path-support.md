# Issue #117 — Actually hash & dedupe files whose absolute path exceeds `PATH_MAX`

## Context

PR #115 made oans **warn** (instead of silently drop) when it meets a file whose
absolute path exceeds `PATH_MAX` (4096). #117 is the follow-through: *actually*
hash and dedupe those files. The blocker is the kernel — `open()`/`statx()`/
`stat()` on an absolute path >`PATH_MAX` return `ENAMETOOLONG`; the only way in
is `openat(dirfd, relative≤PATH_MAX, …)` from a reachable ancestor. The user
chose **full support** (arbitrary-length paths: deep directory chains *and* long
filenames), which is what the existing `tests/integration/test_long_path.py`
repro builds (a chain of 255-char dirs whose parent is itself >`PATH_MAX`).

### Key findings from exploration (they shrink the job vs. the issue's estimate)

The storage/dedupe/prune layer is **already length-agnostic**:
- `files.filename` is `TEXT` (unbounded); `path_hash` uniqueness is on an integer
  hash, not the text — no schema limit. (`src/dbfile.c:283`)
- The store bind uses `sqlite3_bind_text(..., -1, ...)` — the whole NUL-terminated
  string. (`src/dbfile.c:1564`)
- `struct filerec.filename` is already a heap `char*` (`src/filerec.h:46`), loaded
  from the TEXT column with no length cap (`src/dbfile.c:1791,2083`).
- `filerec_open()` (`src/filerec.c:254`) and `dbfile_prune_missing_files()`
  (`src/dbfile.c:2283`) already act on the full string — they just need a
  kernel-reachable open/stat.

So the **full absolute path string is available at every access point** (in-process
at hash time; via the TEXT column at dedupe/prune time). We can therefore derive
the openat-split **on demand from the flat string** — **no structured
prefix/components column, and no `DB_FILE_MINOR` bump** (stays 5.0). This is the
main simplification over the issue's own sketch.

The remaining hard `PATH_MAX` caps are only:
- `struct file.filename[PATH_MAX+1]` — write + change-detection read
  (`src/dbfile.h:68`; set at `src/file_scan.c:1253`, `src/dbfile.c:2045`)
- the walk's skip-guard and full-path syscalls (`src/file_scan.c:986,1033,1039`)
- two **display-only** buffers (`src/run_dedupe.c:135`, `src/progress.h:28`)

## Design

### 1. New shared helper: `src/longpath.{c,h}`

A small, unit-testable module (kept separate so `src/tests.c` can exercise it):

```c
/* Open abspath even if strlen(abspath) > PATH_MAX, by opening a reachable
 * ancestor and openat-walking the remainder. `flags` as for open(2)
 * (e.g. O_RDONLY, or O_RDONLY|O_DIRECTORY for the walk). Returns fd or -1/errno.
 * For paths <= PATH_MAX it is exactly plain open() — callers still gate on
 * length so the hot path is untouched (see below). */
int longpath_open(const char *abspath, int flags);

/* fstatat-based stat that tolerates abspath > PATH_MAX (opens the parent via
 * the same chain, then fstatat(dirfd, basename, ...)). Used by prune. */
int longpath_stat(const char *abspath, struct stat *st);
```

Core algorithm (shared): start from the leading `/`, then repeatedly take the
longest run of remaining components whose joined length ≤ `PATH_MAX` (each
component is ≤ `NAME_MAX`=255, so a chunk always fits), `openat(dirfd, chunk,
O_DIRECTORY|O_PATH|O_CLOEXEC|O_NOFOLLOW-as-appropriate)`, close the previous
dirfd, repeat; `openat` the final component with the caller's `flags`. `O_PATH`
for intermediate dirs keeps it cheap. Simplest-correct fallback if chunking is
fiddly: one component per `openat` (always ≤ NAME_MAX) — optimize to chunks only
if measured.

**Fast-path gate (critical — see CLAUDE.md "single consumer"/hot path):** every
call site keeps the existing `open()/opendir()/stat()` and only falls to the
helper when `strlen(path) > PATH_MAX` (or on an `ENAMETOOLONG` from the plain
call). Normal-length files — i.e. essentially every real workload — run the
identical code as today, so this is perf-neutral (verify with `bench.py`).

### 2. Widen the path-storage buffers

- `struct file.filename[PATH_MAX+1]` → `char *filename` (`src/dbfile.h:68`),
  always heap-owned. Add tiny `file_set_filename(struct file*, const char*)`
  (strdup) + free in the struct's teardown, and update the few writers:
  - `src/file_scan.c:1253` `strncpy(dbfile.filename, path, PATH_MAX)` →
    `file_set_filename(&dbfile, path)` (path is the exact queue string).
  - `src/dbfile.c:2045` read-back `strncpy(dbfile->filename, buf, PATH_MAX)` →
    set from the full `sqlite3_column_text`.
  - Free wherever a stack `struct file dbfile = {0,}` goes out of scope
    (`__scan_file`, `dbfile_describe_file`, and any other `struct file` locals).
    Keep it valgrind-clean — this area is the UAF-prone one.
- `struct file` bind site (`src/dbfile.c:1564`) is unchanged (already `-1`).

### 3. The walk (`process_dir`, `src/file_scan.c:982`)

- Replace `opendir(path)` (line 986) with: if `strlen(path) > PATH_MAX`,
  `fd = longpath_open(path, O_RDONLY|O_DIRECTORY|O_CLOEXEC); dirp = fdopendir(fd)`;
  else `opendir(path)` as today.
- **Delete the skip-guard** (lines 1029–1037) — no longer skip; keep the
  oversized `child` buffer only as far as needed to build names for `fileq_push`/
  `dirq_push` (the queue items are already exact-sized `char path[]`, so long
  strings flow through unchanged). Names can exceed `PATH_MAX`; size `child`
  accordingly (dynamic or a generous `PATH_MAX + NAME_MAX + 2`).
- Change `statx(0, child, 0, …)` (line 1039) → `statx(dirfd(dirp),
  entry->d_name, 0, …)` (dir-fd-relative, mirroring existing `get_dirent_type`
  at `src/file_scan.c:880`) so deep children stat correctly.
- Recursion still goes through `dirq_push(strdup(child))` — **no fds held across
  the queue**; each `process_dir` reopens via the helper. This keeps the queue
  string-based and localizes all chain logic to `longpath_open`.

### 4. Hashing (`__scan_file` / `csum_whole_file`, `src/file_scan.c`)

- Subvol-lookup `open(path, …)` (line 1192) and the hashing
  `open(file->path, …)` (line 1918) → length-gated `longpath_open(...)`.
- Progress copy `strncpy(tprogress->file_path, file->path, PATH_MAX)`
  (line 1879) → see §6.

### 5. Dedupe (`filerec_open`, `src/filerec.c:254`)

- `open(file->filename, O_RDONLY)` → length-gated
  `longpath_open(file->filename, O_RDONLY)`. Runs in the separate
  `oans --hashfile` dedupe process; `file->filename` is the full TEXT — nothing
  else to thread.

### 6. Prune (`dbfile_prune_missing_files`, `src/dbfile.c:2283`)

- `stat(fn, &st)` (line 2310) → length-gated `longpath_stat(fn, &st)`. Without
  this, every long-path row `stat()`s `ENAMETOOLONG` and (since only ENOENT/
  ENOTDIR prune) is *kept* — but it would never have been storable pre-fix; the
  point is correct re-validation on rescan. Preserve the existing
  ENOENT/ENOTDIR-only prune semantics (CLAUDE.md: prune must stay stat-based).

### 7. Display buffers (cosmetic, truncation OK)

- `src/progress.h:28` `file_path[PATH_MAX+1]` and `src/run_dedupe.c:135`
  `clean[PATH_MAX+1]`: these only render the live UI/report. Simplest: keep fixed
  but make the copies **NUL-safe** and truncate long paths for display (e.g.
  show a `…/tail` elision), OR make them dynamic. Not correctness-critical; pick
  truncate-safe to keep the change small. Fix the `strncpy(..., PATH_MAX)`
  no-terminator bug at `src/run_dedupe.c:377` while here.

### 8. Roots & stdin edge cases (leave as warnings, document)

- `scan_file` canonicalizes roots with `realpath()` into `char[PATH_MAX]`
  (`src/file_scan.c:1319`) and `realpath()` itself fails `ENAMETOOLONG` for a
  resolved path >`PATH_MAX`. A *root* that is itself >`PATH_MAX` stays
  unsupported (keep the warning); the deep part is discovered *during the walk*
  from a short root, which is the supported/common case and what the test builds.
- Keep the stdin-list guard (`src/oans.c:511`) and the `oans.c:1434` root
  realpath as-is. Note this scoping in the man page / issue.

## Critical files

- **New:** `src/longpath.c`, `src/longpath.h` (add to `CFILES`/Makefile;
  `src/tests.c` gains unit tests).
- `src/dbfile.h` (struct), `src/dbfile.c` (read-back, prune, store).
- `src/file_scan.c` (`process_dir`, `__scan_file`, `csum_whole_file`).
- `src/filerec.c` (`filerec_open`).
- `src/run_dedupe.c`, `src/progress.h` (display).

## Test plan

**C unit tests (`src/tests.c`, run via `make test`)** for `longpath`:
- `longpath_open` on a normal short path == plain `open` (fd valid, same inode).
- Build a >`PATH_MAX` chain in a tmp dir via incremental `mkdirat`/`chdir` (as
  the Python helper does), `longpath_open` the leaf, read back known contents.
- `longpath_open` a deep **directory** with `O_DIRECTORY` and `fdopendir` it.
- `longpath_stat` returns correct `st_size`/`st_ino` for a deep leaf; returns
  `ENOENT` for a missing deep leaf (prune correctness).
- Error paths: missing intermediate component → `ENOENT`, not `ENAMETOOLONG`.

**Integration tests** — extend `tests/integration/test_long_path.py`
(reuse its `_build_over_pathmax_file` deep-chain builder and the `DuperemoveTest`
helpers: `scan`, `dedupe`, `hf_scalar`/`hf_count`, `assertShared`, `sync`):
- **Flip the existing assertions**: the deep victim is now **hashed** — assert it
  IS present in `files` (`select count(*) … filename like '%victim'` → 1), and
  the "exceeds PATH_MAX" warning is **gone** for it.
- **Dedupe a deep pair:** build two byte-identical files at the deep leaf (or one
  deep + one shallow copy), `dedupe`, `assertDmOk`, `sync`, `assertShared`.
- **No-op rescan:** rescan the deep tree → 0 changes, identical row counts
  (CLAUDE.md invariant).
- **Prune:** scan deep tree, delete the deep victim (via incremental `chdir` +
  `unlink`), rescan → its row is pruned; a sibling long-path file is **kept**
  (guards against `ENAMETOOLONG`-prunes-everything).
- Keep a **long-basename-in-shallow-dir** case too (parent ≤ PATH_MAX, one
  openat) to cover that branch distinctly from the chain.

**Valgrind gate (required for this area):** `make integration-valgrind` — the
`struct file.filename` ownership change is UAF/leak-prone; `.vglogs/` must be
clean (CLAUDE.md "Valgrind"). Also run `scripts/verify.sh`.

**Perf neutrality:** `scripts/bench.py -p realistic --bin base=<master build>
--bin new=./oans` and `-p many` — confirm no regression (the length gate means
normal files never enter the helper).

**Manual end-to-end:** build a deep tree on the btrfs test dir, run
`./oans -rd --hashfile=/tmp/h.db <root>`, confirm the deep file is hashed,
deduped (`compsize`/`assertShared`-style check), and a bare replay run is a no-op.

## Out of scope (documented, not implemented)

- A single **root** whose own realpath >`PATH_MAX` (still warned). Deep trees
  under a short root are fully supported.
- stdin path-list entries >`PATH_MAX-1` (still warned at `src/oans.c:511`).
