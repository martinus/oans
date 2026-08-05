"""Block digests are staged in a bounded batch, not one array per file (#161).

Block hashes are write-only during a scan: each is filled once, in file order,
then only serialized to the hashfile. Holding all of them meant an 8 TiB file at
-b 4K needed ~48 GiB of anonymous memory (2^31 blocks x 24 bytes) purely as a
staging buffer -- an OOM, not a slowdown.

The array is now recycled every BLOCK_BATCH_MAX entries. These tests use the
DUPEREMOVE_BLOCK_BATCH hook to force many flushes on small files; what has to
hold is that flushing changes nothing observable -- same digests, same row
count, same dedupe outcome.

Block hashing only happens under --dedupe-options=partial, which is off by
default, so every test here passes it explicitly.
"""

import os
import unittest

from harness import DuperemoveTest, requires_reflink

PARTIAL = "--dedupe-options=partial"
# 4K blocks over 1 MiB files gives 256 block digests per file, so a batch of
# 16 forces 16 flushes per file -- plenty of boundary crossings.
SMALL_BATCH = {"DUPEREMOVE_BLOCK_BATCH": "16"}
BLOCK_ARGS = ("-b", "4K", PARTIAL)


class BlockBatchingTest(DuperemoveTest):
    def _block_rows(self):
        return self.hf_query(
            "select f.filename, b.loff, quote(b.digest) from blocks b "
            "join files f on f.id = b.fileid order by f.filename, b.loff")

    def _file_digests(self):
        return self.hf_query(
            "select filename, quote(digest) from files order by filename")

    def test_flushing_does_not_change_the_stored_digests(self):
        """The batch boundary must be invisible in the hashfile."""
        self.mkdup("tree/a.bin", "tree/b.bin", 1 << 20)
        self.scan(self.path("tree"), *BLOCK_ARGS)
        self.assertDmOk()
        unbatched_blocks = self._block_rows()
        unbatched_files = self._file_digests()
        self.assertEqual(512, len(unbatched_blocks),
                         "expected 256 block digests per 1 MiB file at -b 4K")

        # Same tree, same options, but flushing every 16 blocks.
        os.unlink(self.hf)
        self.dm("-r", self.path("tree"), *BLOCK_ARGS, env=SMALL_BATCH)
        self.assertDmOk()

        self.assertEqual(unbatched_blocks, self._block_rows(),
                         "batched flushing changed the block digests")
        self.assertEqual(unbatched_files, self._file_digests(),
                         "batched flushing changed the file digests")

    def test_no_duplicate_or_missing_rows_across_flushes(self):
        """Each block is written exactly once, however the batches fall."""
        self.mkrand("tree/a.bin", 1 << 20)
        self.dm("-r", self.path("tree"), *BLOCK_ARGS, env=SMALL_BATCH)
        self.assertDmOk()

        total = self.hf_scalar("select count(*) from blocks")
        distinct = self.hf_scalar("select count(*) from "
                                  "(select distinct fileid, loff from blocks)")
        self.assertEqual(total, distinct, "a block was stored twice")
        self.assertEqual(256, total, "a block went missing across a flush")

    def test_batch_size_does_not_change_the_row_count(self):
        """Sweep the boundary: a size that divides evenly, and one that doesn't."""
        self.mkrand("tree/a.bin", 1 << 20)
        counts = {}
        for batch in ("7", "16", "64", "255", "256", "257"):
            os.unlink(self.hf) if os.path.exists(self.hf) else None
            self.dm("-r", self.path("tree"), *BLOCK_ARGS,
                    env={"DUPEREMOVE_BLOCK_BATCH": batch})
            self.assertDmOk(f"scan failed at batch={batch}")
            counts[batch] = self.hf_scalar("select count(*) from blocks")
        self.assertEqual({256}, set(counts.values()),
                         f"row count varied with batch size: {counts}")

    @requires_reflink
    def test_partial_dedupe_still_works_after_flushing(self):
        """The rows are not just present, they are still usable."""
        a, b = self.mkdup("tree/a.bin", "tree/b.bin", 1 << 20)
        self.sync()
        self.dm("-rd", *BLOCK_ARGS, self.path("tree"), env=SMALL_BATCH)
        self.assertDmOk()
        self.assertShared(a, b, "partial dedupe failed after batched flushes")

    @requires_reflink
    def test_data_is_unchanged(self):
        self.mkdup("tree/a.bin", "tree/b.bin", 1 << 20)
        self.sync()
        before = self.tree_digest(self.path("tree"))
        self.dm("-rd", *BLOCK_ARGS, self.path("tree"), env=SMALL_BATCH)
        self.assertDmOk()
        self.assertEqual(before, self.tree_digest(self.path("tree")),
                         "file contents changed")

    def test_tiny_file_never_trips_the_inlined_assert(self):
        """An inlined file must not have flushed early.

        csum_whole_file() skips storing blocks for an inlined file entirely; if
        such a file had already flushed a batch, that check would be dropping
        only the tail while earlier batches sat committed. The batch cap is far
        above the inline limit so it cannot happen -- this pins that a
        one-block-batch run over tiny files still exits cleanly.
        """
        for i in range(4):
            self.write(f"tree/tiny{i}.bin", b"x" * 100)
        self.dm("-r", self.path("tree"), *BLOCK_ARGS,
                env={"DUPEREMOVE_BLOCK_BATCH": "1"})
        self.assertDmOk()


if __name__ == "__main__":
    unittest.main()
