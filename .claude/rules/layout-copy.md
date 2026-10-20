---
paths:
  - "src/file_scan.c"
  - "src/fiemap.{c,h}"
  - "src/dbfile.c"
  - "tests/integration/test_layout_copy.py"
---

# Snapshot-aware scan: copy hashes for an identical layout (#206)

Files with the same fiemap records (`fe_logical`, `fe_physical`, `fe_length`,
`fe_flags`) and size get the donor's digest, extent and block hashes copied
(4 snapshots cold: 2.36 s -> 0.65 s).

- **Misses are free, false hits are catastrophic.** All three bars must pass:
  `fiemap_layout_key()` refuses records it cannot vouch for (DATA_INLINE,
  UNKNOWN, DELALLOC, DATA_ENCRYPTED, none at all); the key is a 128-bit digest
  of every record; `dbfile_layout_matches()` re-checks the donor's stored
  `(loff, poff, len)` triples. The donor's `extents` rows are its record
  array, so vanished rows (aborted batch, hardlink cascade, `only_whole_files`)
  are a miss by construction.
- **Never normalize or merge records**: other record boundaries (#186) must
  miss. SHARED and LAST are masked out (unrelated dedupe changes SHARED; LAST
  is positional).
- **ENCODED refuses the key (#287)**: a compressed extent reports its own
  address whatever offset a file uses, so clones of different halves look
  identical. `test_layout_copy.py` builds that with `FICLONERANGE`.
- **Donors are this run's files only**, so hashing options (`--skip-zeroes`,
  block size) match. Re-check and copy run on the `scan_writer` connection in
  the open batch, so an uncommitted donor is visible; another connection would
  need a fallback.
- **`LAYOUT_COPY_MIN_SIZE = READ_BUF_LEN` (1 MiB), checked before building the
  key**: map RSS on huge trees (#208), and a post-key gate made small files and
  kill-switch runs pay for a useless hash.
- Donor table is open-addressed with inline slots, like `seen_inodes`
  (`GHashTable` ~50 B/entry vs 24).
- Not gated on `--hashfile` (in-memory database works the same). A hit needs a
  *finished* donor; tests use `--io-threads=1`.
- `DUPEREMOVE_NO_LAYOUT_COPY=1` is the kill switch. **The test gate is
  byte-identical output against a run with it set** (`test_layout_copy.py`),
  not a counter.
