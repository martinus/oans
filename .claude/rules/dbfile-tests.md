---
paths:
  - "src/dbfile.{c,h}"
  - "tests/unit/test_dbfile.c"
  - "tests/unit/tu_dbfile.c"
---

# dbfile unit tests

`dbfile_open_handle(NULL)` opens the shared-cache in-memory SQLite that a run
without `--hashfile` ships with, so `dbfile.c` needs no filesystem or scan.

- **All `memdb()` handles share one database**; a second
  `dbfile_open_handle(NULL)` is not empty. Ask what a fresh hashfile answers
  before the test stores anything.
- Fixtures need distinct values. `dbfile_get_run_summary` reads five error
  buckets by adjacent column index, so equal (0) buckets hide a dropped read.
  `dbfile_load_scan_config` reads both size limits through one scratch
  variable: delete only the additive key, or deleting `mfs = 0` (`max`
  silently becoming `min`) survives.
- A `perror_sqlite` + `goto out` pair (e.g. all of `dbfile_describe_file`'s
  survivors) is residue; the next step is a fault-injection seam.
- Test what fails silently (a vanished row, a digest from the wrong donor, a
  replay adopting a dead default: all exit 0). `bugs/dbfile.txt` puts them back.
- `dbfile_store_file_info()` writes no digest, `dbfile_update_scanned_file()`
  does; a digest-less row is deleted by the startup prune.
  `dbfile_load_scan_config()` returns **1** when a config was stored, 0 for
  none (`== 0` reads success as failure). `dbfile_load_checkpoint()` copies
  into caller-owned buffers; a zeroed `struct scan_checkpoint` writes through
  NULL.
- `mu_check` expands to a bare `return;` (`minunit.h:149`, like
  `prop_check`): never use it in a helper returning a value (clang refuses),
  and an `x && ...` guard after `mu_check(x != NULL)` is dead. Fixture-broken
  helpers `abort()` (`gs_hit()`, `exec()`). `mu_assert_string_eq` prints the
  actual value.

## The dedupe-phase loaders

Tests need `memdb()` and `mkfilerec()`/`free_all_filerecs()` together.

- `GET_DUPLICATE_FILES` elects the target read-only first (#172: the kernel
  will not write into one), then fewest extents, then lowest id, from
  scan-time columns so **every window elects the same target** (#197).
  Members load at `poff = 0` and skip `clean_deduped()` (#186).
- **The winner must be one only the real ranking picks.** A lowest-id,
  tidiest, oldest winner also passes a pre-#197 `min(id)` election; in the
  tie-break case, names, inos and ids ascending together pass a comparator
  with no tie-break.
- **Row order is the election**: `order by is_target desc, f.id`,
  `insert_extent_list_free()` appends, `dedupe_extent_list()` takes
  `list_first_entry()`. Guard that, not a marker: `de_anchored` was written,
  never read, and removed (#237); a reader for it would restore the per-pass
  re-election #197 removed. `test_whole_file_dupes_load_at_poff_zero` and
  `test_the_whole_file_target_prefers_readonly_then_least_fragmented` catch a
  dropped `order by`.
- String literals are not mutated, so `GET_DUPLICATE_*` SQL is guarded only
  by `bugs/dbfile.txt`. The inlined-file filter in the `grp` CTE shows only
  when the group exists because of the inlined file.
- `target_of()` wrapped `list_first_entry()`, which never returns NULL (an
  empty list gives a head-derived pointer), so `mu_check(t != NULL)` held
  whatever the loader did. A helper must be able to fail.
