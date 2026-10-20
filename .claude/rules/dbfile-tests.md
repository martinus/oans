---
paths:
  - "src/dbfile.{c,h}"
  - "tests/unit/test_dbfile.c"
  - "tests/unit/tu_dbfile.c"
---

# The hashfile is unit-testable, in memory

`dbfile_open_handle(NULL)` opens a shared-cache in-memory SQLite - the same path
a run without `--hashfile` takes - so `dbfile.c` needs no filesystem, no btrfs
and no scan to test. That is not a test-only seam; it is the in-memory mode
oans already ships. Coverage went 0% → 47% on eight tests, and a whole-file
sweep from **901 survivors to 785**.

- **`memdb()` handles all share one database.** Shared-cache in-memory means a
  second `dbfile_open_handle(NULL)` is the *same* database, not an empty one -
  so "what does this answer against a fresh hashfile" has to be asked before
  the test stores anything, and a test that opens a second handle expecting a
  clean slate silently asserts against the first one's rows.
- **Read the survivors by function here too, and twice it changed the fix.**
  `dbfile_get_run_summary` barely moved on the second sweep, 22 → 21, and the
  grouping said why: its five error buckets are adjacent columns read by
  index, and the fixture had three of them at 0 - indistinguishable from each
  other *and* from a read that was dropped. Distinct values fixed it.
  `dbfile_load_scan_config` still let `mfs = 0` be deleted, because the
  fixture deleted *both* size-limit keys; the two limits are read through one
  scratch variable, so only deleting the additive one exposes `max` silently
  becoming `min`. In both cases the test looked thorough and the number said
  otherwise.
- **A `perror_sqlite` + `goto out` pair is residue, not a hole.** After the
  strengthening, all ten of `dbfile_describe_file`'s remaining survivors are
  that shape - a bind or step failure no unit test can provoke. Triage to
  that point and stop; the next honest move is a fault-injection seam, not a
  bigger fixture.

- **What is worth testing there is what fails silently.** A hashfile is a cache,
  so nothing downstream validates it: a row that vanished, a digest copied from
  the wrong donor and a replay that adopted a dead default all produce a run
  that exits 0 and looks exactly like a correct one. `bugs/dbfile.txt` puts
  eight of those back.
- **Three API shapes that a test gets wrong first.** `dbfile_store_file_info()`
  writes no digest - `dbfile_update_scanned_file()` does, and the digest is
  precisely what tells a finished file from one an interrupted run left partway,
  so a helper that skips it builds rows the startup prune deletes.
  `dbfile_load_scan_config()` returns **1** for "a config was stored" and 0 for
  "none", so `== 0` reads success as failure. And `dbfile_load_checkpoint()`
  *copies into buffers the caller owns* - a zeroed `struct scan_checkpoint` is a
  write through NULL, which is now said in the header.
- **`mu_check()` cannot be used in a helper that returns a value.** It expands
  to a bare `return;`; gcc warns, clang refuses. Conditions that mean "the test
  itself is broken" `abort()` instead, the way `gs_hit()` already did.

## The dedupe-phase loaders

Where the hashfile meets the in-memory model. **155 mutants, 0% killed → 73%**
(41 survivors) on ten tests, and they need *both* fixtures at once - `memdb()`
and `mkfilerec()`/`free_all_filerecs()` - which is why they came last.

- **`GET_DUPLICATE_FILES` is the richest thing in the file**, because its
  invariants are the ones with issue numbers: the target election ranks
  read-only first (#172 - the kernel will not write into one, so it must be the
  source), then fewest extents, then lowest id, all from columns fixed at scan
  time so that **every window elects the same target** (#197). Members load at
  `poff = 0`, which is why they skip `clean_deduped()` (#186).
- **A fixture whose winner leads on every column proves nothing.** The first
  #197 test had one that was lowest-id *and* tidiest *and* oldest, so a
  pre-#197 `min(id)` election passed it too. The winner must be one that only
  the real ranking picks - and the same trap sat in the tie-break case, where
  names, inos and ids all ascended together so a comparator with no tie-break
  at all still answered correctly.
- **`de_anchored` is gone, and the way it was defended is the lesson (#237).**
  It was written from `GET_DUPLICATE_FILES`'s `is_target` column and *never
  read* — deleting the write left every other test green. The response at the
  time was to assert the flag itself, on the argument that the assertion was
  "how the disconnection would be noticed". That reasoning is backwards: with
  no reader there is no connection to break, so what the assertion pinned was
  the existence of dead state, and a bug block guarding it could only ever be
  caught by that one assertion. Worse, state that *looks* like a guard invites
  someone to restore the missing reader and reintroduce the per-pass
  re-election #197 removed.
  - **Row order is the actual mechanism.** `order by is_target desc, f.id`
    carries the rank out of the query, `insert_extent_list_free()` appends, and
    `dedupe_extent_list()` takes `list_first_entry()`. So the target being first
    *is* the election — there is nothing else to check, and `target_of()` in the
    #197 test was already checking it.
  - **The trade is measurable, which is why it is the right one.** The bug block
    that broke the flag was caught only by the assertion written for it; the one
    that replaced it — dropping the `order by` — is caught by
    `test_whole_file_dupes_load_at_poff_zero` and
    `test_the_whole_file_target_prefers_readonly_then_least_fragmented`, tests
    that are about behaviour. Prefer a guard on the mechanism over a guard on a
    marker that merely accompanies it.
- **The tool does not mutate string literals**, so the `GET_DUPLICATE_*` SQL
  cannot be swept - a test that only exercises SQL filtering guards against
  hand edits but moves no kill rate. `bugs/dbfile.txt` is where those go, and
  one of them showed a fixture hole a C sweep never could: dropping the
  inlined-file filter from the `grp` CTE is invisible with two real copies,
  because the group qualifies on them alone. It shows only when the group
  exists *because* of the inlined file.
- **A helper that cannot fail hides the test.** `target_of()` wrapped
  `list_first_entry()`, which on an empty list returns a pointer derived from
  the head rather than NULL - so every `mu_check(t != NULL)` around it held
  whatever the loader did. Helpers that mean "the fixture is broken" `abort()`,
  like `gs_hit()` and `exec()`.
- **`mu_check` returns on failure** (`minunit.h:149`), exactly like
  `prop_check`. So an `x && ...` guard after `mu_check(x != NULL)` is dead;
  `mu_assert_string_eq` is better still where the value matters, since it
  prints what was actually there.
