---
paths:
  - "src/file_scan.c"
  - "src/fiemap.{c,h}"
  - "src/dbfile.c"
  - "tests/integration/test_layout_copy.py"
---

# Snapshot-aware scan: copy hashes for an identical layout (#206)

Hashing N snapshots of a subvolume read N× the data. Two files whose fiemaps
describe **the same records** (`fe_logical`, `fe_physical`, `fe_length`,
`fe_flags`, over the same size) are backed by the same stored bytes, so the
second one's digest, extent hashes and block hashes are **copied** instead of
computed. Measured on a 4-snapshot 7.5 GiB tree, cold: **2.36 s → 0.65 s**
(3.6×, against a ceiling of 4×). Flat on `realistic`/`many`/`big` — same
medians, same RSS.

- **Misses are free, false hits are catastrophic.** A miss costs one redundant
  hash; a wrong copy stores a digest of bytes the file never had, and nothing
  downstream could tell that from a file with no duplicate. Hence three
  independent bars, all of which must pass: `fiemap_layout_key()` refuses any
  record whose address it cannot vouch for (DATA_INLINE, UNKNOWN, DELALLOC,
  DATA_ENCRYPTED, or no extents at all), the key is a 128-bit digest of *every*
  record, and `dbfile_layout_matches()` then re-checks the donor's stored
  `(loff, poff, len)` triples one at a time.
  - **Never normalize or merge records to chase more hits.** The same storage
    described with different record boundaries (#186) must read as a *miss*.
  - SHARED and LAST are masked out of the key: the first changes when an
    unrelated file is deduped, the second is positional. Neither says anything
    about content.
  - **ENCODED refuses the key (#287).** These notes used to say compressed
    extents were fine because only `fe_physical` equality is compared. It is
    not enough: a compressed extent reports the *extent's* address whatever
    offset of it a file references. Measured on btrfs, kernel 7.1: clone the
    first half of sixteen 128 KiB compressed extents into X and the second
    half into Y, and X and Y report identical records, hold different bytes,
    and Y was stored with X's digest. The offset is visible only to root, so
    compressed data gets no layout copy; snapshots of compressed trees are
    hashed like everything else. `test_layout_copy.py` builds that layout
    with `FICLONERANGE`.
- **The re-check needs no extra state: the donor's `extents` rows *are* its
  fiemap record array.** So the in-memory map holds only a key and a file id,
  and a donor whose rows vanished (aborted batch, hardlink cascade, only_whole_
  files) verifies as a miss by construction rather than needing a guard.
- **Donors are this run's files only.** Block size, `--skip-zeroes` and the rest
  of the hashing options are then the same by construction; an older hashfile's
  rows might have been produced under different ones.
- **No visibility hazard, because every scan write goes through the one shared
  `scan_writer` connection.** The re-check and the copy run on that connection
  inside the open batch, so a donor hashed seconds ago is visible before any
  commit. (The issue anticipated needing a fallback for this; a different
  connection *would* need one.)
- **`LAYOUT_COPY_MIN_SIZE = READ_BUF_LEN` (1 MiB), checked *before* the key is
  built.** The map is one entry per candidate donor, so on a multi-million-file
  tree it would be tens of MB of RSS for nothing (see #208); below one read
  buffer the file is a single I/O and the bookkeeping costs more than it saves.
  Gating after the key — as the first cut did — meant every small file, and
  every file at all under the kill switch, paid for a hash that could never
  match, since the size is part of the key.
- **The donor table is open-addressed with inline slots, like `seen_inodes`** —
  a `GHashTable` node plus a malloc per entry is ~50 B where the entry is 24,
  and there is one per large file. Same reason that set stopped using one.
- **Nothing gates this on `--hashfile`.** A run without one keeps its rows in an
  in-memory database that the copy reads and writes identically; measured, the
  dedupe outcome is the same with and without. Only `DUPEREMOVE_NO_LAYOUT_COPY`
  turns it off.
- **A hit needs the donor to have *finished*.** With N workers, a file and its
  snapshot are often hashed side by side and neither can donate — measured 74-82%
  hit rate on small trees, and it only improves with tree size. Fine in
  production; tests pin it with `--io-threads=1`.
- `DUPEREMOVE_NO_LAYOUT_COPY=1` is the kill switch. **The gate is byte-identical
  output against a run with it set** (`test_layout_copy.py`), not "the counter
  went up" — that is the only definition of correct here. Off automatically
  without `--hashfile`.
