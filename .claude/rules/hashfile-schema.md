---
paths:
  - "src/dbfile.{c,h}"
  - "tests/integration/test_appid.py"
  - "tests/integration/test_hashfile_mode.py"
  - "tests/integration/test_readonly_reports.py"
---

# Hashfile identity & schema version

Schema version is `DB_FILE_MAJOR.MINOR` in `dbfile.h` (5.0; forked at upstream
4.1, jumped to 5.0 as a clean break). Every hashfile is stamped `PRAGMA
application_id = OANS_APP_ID` ("oans").

- **`dbfile_identify()` runs before anything writes to the file (#275).** A
  mistyped `--hashfile` can name another program's database, and every step
  of an open writes: the journal mode, the tables, the brand, and on a failed
  check the unlink that recreates it. It used to brand any file whose `config`
  table looked empty, so another program's database got oans's tables; and a
  database with a `config` table of its own failed the version check and was
  **unlinked**. Now:
  - empty (no schema, application_id 0): new, branded;
  - our brand: ours, and a failed `dbfile_check()` still recreates it (a cache);
  - unbranded with a `config` row `version_major`: a duperemove or pre-brand
    oans hashfile, rebuilt by a scan and refused by a report;
  - anything else, including a *different* brand: refused untouched, exit 1.
  - **A lock is not an answer.** The busy timeout is set before the first
    question, and only `SQLITE_NOTADB` reads as foreign; any other error is
    reported as itself. Reading every error as "foreign" refused a hashfile
    another oans was writing.
- **A report writes nothing.** It skips `PRAGMA journal_mode = WAL` (stored
  in the file; SQLite reads a WAL file as one anyway) and prepares only the
  three `count_*` statements `dbfile_get_stats()` uses. It does not run
  `create_tables()`, so every other statement may name a column or table an
  older hashfile lacks: preparing them all failed `--json` on any hashfile no
  scan had upgraded yet. `run_history` is the same trap in the report's own
  queries (no table before v1.4, two columns short before v1.7), so a report
  on such a file gets a `TEMP VIEW run_history` that reads the missing
  columns as 0. The temp schema is not the file.

- **Bump `DB_FILE_MINOR` only for a change an old binary could misread.**
  `dbfile_check()` rejects a differing `minor`, discarding and rebuilding the
  file (a full re-scan). Do **not** bump for purely *additive* changes (a new
  `CREATE TABLE IF NOT EXISTS`, or an optional `config` key): `create_tables()`
  runs every open, so additive tables appear on old files and old binaries ignore
  the extras. Self-describing and run-history were both added this way, left at
  `5.0`. Removing a key is equally safe: `--autotune`'s orphaned
  `autotune_io_threads` row is simply never read again (no bump, no migration).
- A from-scratch build sets `hashfile_rebuilt` → `dbfile_maybe_vacuum()` forces a
  one-off `VACUUM` (a fresh build is at insert density, ~15-20% larger
  un-vacuumed). Incremental runs only VACUUM at ≥25% free.
