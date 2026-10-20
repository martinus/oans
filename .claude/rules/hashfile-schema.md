---
paths:
  - "src/dbfile.{c,h}"
  - "tests/integration/test_appid.py"
  - "tests/integration/test_hashfile_mode.py"
  - "tests/integration/test_readonly_reports.py"
---

# Hashfile identity & schema version

Version is `DB_FILE_MAJOR.MINOR` in `dbfile.h` (`5.0`); every hashfile gets
`PRAGMA application_id = OANS_APP_ID` ("oans").

- **`dbfile_identify()` runs before anything writes (#275)**; every open step
  writes, and a mistyped `--hashfile` may name another program's database.
  Empty (no schema, application_id 0): new, branded. Our brand: ours, a failed
  `dbfile_check()` recreates it. Unbranded with `config` row `version_major`:
  duperemove/pre-brand oans, rebuilt by a scan, refused by a report. Anything
  else, including another brand: refused untouched, exit 1.
  - **A lock is not an answer**: set the busy timeout first; only
    `SQLITE_NOTADB` means foreign, other errors are reported as themselves.
- **A report writes nothing**: no `PRAGMA journal_mode = WAL`, no
  `create_tables()`, only the three `count_*` statements `dbfile_get_stats()`
  uses (others may name columns old hashfiles lack; broke `--json`). For old
  `run_history` (no table before v1.4, two columns short before v1.7) it
  creates a `TEMP VIEW run_history` reading missing columns as 0.
- **Bump `DB_FILE_MINOR` only for changes an old binary could misread**:
  `dbfile_check()` rebuilds on a differing `minor` (full re-scan). Never for
  additive changes (`CREATE TABLE IF NOT EXISTS`, optional `config` key;
  `create_tables()` runs every open) or removed keys (`autotune_io_threads`,
  left by `--autotune`).
- A from-scratch build sets `hashfile_rebuilt` -> `dbfile_maybe_vacuum()`
  forces one `VACUUM` (un-vacuumed is ~15-20% larger); incremental runs only
  at >=25% free.
