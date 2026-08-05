"""Reclaiming block hashes a hashfile no longer uses (#160).

Block hashes serve only --dedupe-options=partial, and they are the bulk of a
hashfile that has them (a fully-mapped 1 TiB at -b 4K stores ~8 GiB of block
digests). They are dropped per file by dbfile_remove_hashes() when a file is
re-hashed -- but the whole point of a hashfile is that unchanged files are *not*
re-hashed, so on a stable tree a user who ran partial once carries those rows
forever with nothing reading them.

#160 asked for an option to not write them. That already exists: block hashing
is off unless --dedupe-options=partial is passed. What was missing is a way to
get rid of the ones already written.
"""

import os
import unittest

from harness import DuperemoveTest

PARTIAL = ("-b", "4K", "--dedupe-options=partial")
PLAIN = ("-b", "4K")


class PruneBlockHashesTest(DuperemoveTest):
    def _seed_with_partial(self, size=1 << 22):
        """A hashfile carrying block hashes from a partial run."""
        self.mkdup("tree/a.bin", "tree/b.bin", size)
        self.sync()
        self.dm("-r", self.path("tree"), *PARTIAL)
        self.assertDmOk()
        self.assertGreater(self.hf_count("blocks"), 0,
                           "partial run stored no block hashes")

    def test_default_scan_stores_no_block_hashes(self):
        """The literal ask of #160 is already the default, at any blocksize."""
        self.mkdup("tree/a.bin", "tree/b.bin", 1 << 20)
        for bs in ("4K", "128K"):
            os.path.exists(self.hf) and os.unlink(self.hf)
            self.dm("-r", self.path("tree"), "-b", bs)
            self.assertDmOk()
            self.assertEqual(0, self.hf_count("blocks"),
                             f"-b {bs} stored block hashes without partial")

    def test_blocksize_does_not_change_extent_or_file_digests(self):
        """#160's rationale for wanting 4K blocks without partial.

        The claim is that matching -b to the filesystem block size improves
        extent matching. It cannot: the extent digest is computed over the
        fiemap extent's own bytes, independent of -b. Pinning that here so the
        reasoning does not have to be re-derived from the code.
        """
        self.mkdup("tree/a.bin", "tree/b.bin", 1 << 21)
        self.sync()
        digests = {}
        for bs in ("4K", "128K"):
            os.path.exists(self.hf) and os.unlink(self.hf)
            self.dm("-r", self.path("tree"), "-b", bs)
            self.assertDmOk()
            digests[bs] = (
                self.hf_query("select f.filename, e.loff, e.len, quote(e.digest) "
                              "from extents e join files f on f.id = e.fileid "
                              "order by 1, 2"),
                self.hf_query("select filename, quote(digest) from files "
                              "order by filename"),
            )
        self.assertEqual(digests["4K"], digests["128K"],
                         "-b changed the extent or file digests")

    def test_block_rows_survive_a_non_partial_rescan(self):
        """The gap itself: nothing reclaims them while files are unchanged."""
        self._seed_with_partial()
        before = self.hf_count("blocks")

        self.dm("-r", self.path("tree"), *PLAIN)
        self.assertDmOk()
        self.assertEqual(before, self.hf_count("blocks"),
                         "this test is stale: block rows are now reclaimed "
                         "by an ordinary rescan")

    def test_prune_drops_them_and_shrinks_the_file(self):
        self._seed_with_partial()
        size_before = os.path.getsize(self.hf)

        self.dm("--prune-block-hashes")
        self.assertDmOk()
        self.assertEqual(0, self.hf_count("blocks"))
        self.assertLess(os.path.getsize(self.hf), size_before,
                        "hashfile did not shrink after pruning")

    def test_reported_size_reflects_the_real_file(self):
        """In WAL mode the main file only shrinks at checkpoint.

        stat()ing before closing the handle reports the pre-VACUUM size and
        claims "0 B freed" right after reclaiming most of the file.
        """
        self._seed_with_partial()
        out = self.dm("--prune-block-hashes")
        self.assertDmOk()
        self.assertIn("freed", out)
        self.assertNotIn("(0 B freed)", out,
                         "reported a saving of zero after a real one")

    def test_prune_is_idempotent(self):
        self._seed_with_partial()
        self.dm("--prune-block-hashes")
        out = self.dm("--prune-block-hashes")
        self.assertDmOk()
        self.assertIn("nothing to prune", out.lower())

    def test_pruning_keeps_the_hashfile_usable(self):
        """Only block hashes go: files and extents must survive untouched."""
        self._seed_with_partial()
        files_before = self.files_fingerprint()
        extents_before = self.extents_fingerprint()

        self.dm("--prune-block-hashes")
        self.assertDmOk()
        self.assertEqual(files_before, self.files_fingerprint())
        self.assertEqual(extents_before, self.extents_fingerprint())

        # And a later rescan is still incremental, not a full re-hash.
        self.dm("-r", self.path("tree"), *PLAIN)
        self.assertDmOk()
        self.assertEqual(extents_before, self.extents_fingerprint())

    def test_stats_flags_unused_block_hashes(self):
        self._seed_with_partial()
        self.dm("-r", self.path("tree"), *PLAIN)   # stored config drops partial

        out = self.dm("--stats")
        self.assertIn("--prune-block-hashes", out,
                      "--stats did not surface the reclaimable rows")

    def test_stats_does_not_flag_a_hashfile_still_using_partial(self):
        """No false positive: these rows are in active use."""
        self._seed_with_partial()
        out = self.dm("--stats")
        self.assertNotIn("--prune-block-hashes", out,
                         "flagged block hashes the stored config still uses")

    def test_is_mutually_exclusive_with_the_other_report_modes(self):
        self._seed_with_partial()
        out = self.dm("--prune-block-hashes", "--stats")
        self.assertIn("use only one of", out)
        self.assertNotEqual(0, self.rc)

    def test_requires_a_hashfile(self):
        out = self.dm("--prune-block-hashes", hashfile=False)
        self.assertIn("--hashfile", out)
        self.assertNotEqual(0, self.rc)


if __name__ == "__main__":
    unittest.main()
