"""Sparse files, holes, unaligned tails, and empty/zero files."""

import errno
import os
from harness import DuperemoveTest, requires_reflink


class SparseTest(DuperemoveTest):
    def test_make_sparse_really_punches_a_hole(self):
        """The gap is in the extent map, not merely skipped by the writer.

        Seeking over a gap leaves a hole on btrfs and ext4, but XFS reserves
        blocks past the end of a buffered extending write - for these sizes,
        about the size of the gap - so the gap can come back mapped and the
        file has no hole at all on half the CI matrix. Every other assertion in
        this file would then pass by not being about anything (#242).

        SEEK_HOLE/SEEK_DATA ask the kernel what the map really is, which is the
        only way to tell a punched hole from a gap the filesystem filled in.
        """
        head, hole, tail = 65536, 131072, 40000
        p = self.make_sparse("tree/sp", os.urandom(head), hole, os.urandom(tail))
        self.sync()
        with open(p, "rb") as f:
            fd = f.fileno()
            self.assertEqual(head, os.lseek(fd, 0, os.SEEK_HOLE),
                             "the hole begins where the head ends")
            self.assertEqual(head + hole, os.lseek(fd, head, os.SEEK_DATA),
                             "and ends where the tail begins")
        self.assertEqual(head + hole + tail, os.stat(p).st_size,
                         "punching kept the file size")

    def test_a_punch_that_fails_is_raised_not_swallowed(self):
        """The test above can only fail on XFS - btrfs and ext4 leave a hole
        whether or not anything punched - so this pins the other half of the
        guarantee, which every filesystem can show: a punch that did not happen
        must not pass for one. tests/unit/fixtures.h can ignore the return
        value because it aborts on the following ftruncate anyway; here the
        file would simply come back without its hole and nothing would say so.
        """
        from harness import punch_hole
        p = self.write("tree/ro", os.urandom(65536))
        fd = os.open(p, os.O_RDONLY)           # fallocate needs it writable
        try:
            with self.assertRaises(OSError) as caught:
                punch_hole(fd, 0, 4096)
        finally:
            os.close(fd)
        self.assertEqual(errno.EBADF, caught.exception.errno)

    def test_sparse_file_scans(self):
        self.make_sparse("tree/sp", os.urandom(65536), 131072, os.urandom(40000))
        self.scan(self.path("tree"))
        self.assertDmOk()
        self.assertEqual(1, self.hf_count("files"))
        self.assertGreater(self.hf_count("extents"), 0, "sparse file has recorded extents")

    def test_sparse_identical_same_digest(self):
        head, tail = os.urandom(65536), os.urandom(40000)
        self.make_sparse("tree/a", head, 131072, tail)
        self.make_sparse("tree/b", head, 131072, tail)   # identical, independent
        self.scan(self.path("tree"))
        self.assertDmOk()
        da = self.hf_scalar("select quote(digest) from files where filename like '%/a'")
        db = self.hf_scalar("select quote(digest) from files where filename like '%/b'")
        self.assertEqual(da, db, "identical sparse files share a digest")

    def test_empty_files(self):
        self.write("tree/empty1", b"")
        self.write("tree/empty2", b"")
        self.mkrand("tree/data", 8000)
        self.scan(self.path("tree"))
        self.assertDmOk()
        # Whether empty files get a row is a policy detail; the invariant that
        # matters is a clean run with the non-empty file recorded.
        self.assertEqual(
            1, self.hf_scalar("select count(*) from files where filename like '%/data'"),
            "non-empty file recorded")

    def test_trailing_hole_scans(self):
        # A file whose last extent ends before EOF (data then a big hole).
        # FIEMAP omits the hole, so walking extents past the last one used to
        # error ("unable to get extent") and abandon the file (upstream #374).
        self.make_trailing_hole("tree/th", os.urandom(200000), 4 * 1024 * 1024)
        self.scan(self.path("tree"))
        self.assertDmOk()
        self.assertNotIn("unable to get extent", self.out)
        self.assertNotIn("changed", self.out)
        self.assertEqual(1, self.hf_count("files"), "trailing-hole file recorded")
        self.assertEqual(
            0, self.hf_scalar("select count(*) from files where digest is null"),
            "trailing-hole file digested (not abandoned)")

    def test_trailing_hole_to_unaligned_eof_scans(self):
        # Same as above but with an EOF that is NOT block-aligned, the shape of
        # a real sparse torrent stub. hole_run_length() floored the hole run to
        # a block, so the final partial block was never consumed and the file
        # was reported "changed while hashing" and skipped, forever.
        self.make_trailing_hole("tree/th", os.urandom(200000),
                                4 * 1024 * 1024 + 91943)
        self.scan(self.path("tree"))
        self.assertDmOk()
        self.assertNotIn("changed", self.out)
        self.assertEqual(1, self.hf_count("files"),
                         "unaligned trailing-hole file recorded")
        self.assertEqual(
            0, self.hf_scalar("select count(*) from files where digest is null"),
            "unaligned trailing-hole file digested (not abandoned)")

    def test_hole_only_file_unaligned_eof(self):
        # The same flooring bug with no data at all: a fully sparse file whose
        # size is not a multiple of the block size.
        self.make_trailing_hole("tree/hole", b"", 8 * 1024 * 1024 + 1)
        self.scan(self.path("tree"))
        self.assertDmOk()
        self.assertNotIn("changed", self.out)
        self.assertEqual(1, self.hf_count("files"), "hole-only file recorded")
        self.assertEqual(
            0, self.hf_scalar("select count(*) from files where digest is null"),
            "hole-only file still digested")

    @requires_reflink
    def test_trailing_hole_dedupes_and_preserves_data(self):
        data = os.urandom(240000)
        a = self.make_trailing_hole("tree/a", data, 8 * 1024 * 1024)
        b = self.make_trailing_hole("tree/b", data, 8 * 1024 * 1024)
        self.sync()
        before = open(a, "rb").read()

        self.dedupe(self.path("tree"))
        self.assertDmOk()
        self.sync()

        self.assertShared(a, b, "identical trailing-hole files dedupe")
        self.assertEqual(before, open(a, "rb").read(), "data intact after dedupe")

    def test_large_hole_only_file(self):
        # A big fully-sparse file (issue #87: `truncate -s 1T`) maps no extents,
        # so it must record no extents and hash no blocks, yet still be digested.
        # Before the hole-skip it read and hashed every zero byte (ETA minutes).
        p = self.write("tree/hole", b"")
        os.truncate(p, 8 * 1024 * 1024 * 1024)  # 8 GiB, entirely a hole
        self.scan(self.path("tree"))
        self.assertDmOk()
        self.assertEqual(1, self.hf_count("files"), "hole-only file recorded")
        self.assertEqual(0, self.hf_count("extents"), "no extents stored for a hole")
        self.assertEqual(
            0, self.hf_scalar("select count(*) from files where digest is null"),
            "hole-only file still digested")

    def test_interior_hole_layout_distinct_digest(self):
        # Same data blocks, same size, different hole placement. Skipping hole
        # bytes must not collapse these to one digest: the file checksum folds
        # each hole's (offset, length) so layout still matters (issue #87).
        block = 131072
        A, B = os.urandom(65536), os.urandom(65536)

        p = self.path("tree/p")            # A . . B  (holes in the middle/tail)
        with open(p, "wb") as f:
            f.write(A); f.seek(3 * block); f.write(B)
        os.truncate(p, 4 * block)

        q = self.path("tree/q")            # A B . .  (holes at the tail)
        with open(q, "wb") as f:
            f.write(A); f.seek(block); f.write(B)
        os.truncate(q, 4 * block)

        self.scan(self.path("tree"))
        self.assertDmOk()
        dp = self.hf_scalar("select quote(digest) from files where filename like '%/p'")
        dq = self.hf_scalar("select quote(digest) from files where filename like '%/q'")
        self.assertNotEqual(dp, dq, "different sparse layout -> different digest")

    def test_unaligned_sizes(self):
        self.mkrand("tree/a", 131072 + 1)
        self.mkrand("tree/b", 262144 + 4095)
        self.mkrand("tree/c", 1)
        self.scan(self.path("tree"))
        self.assertDmOk()
        self.assertEqual(3, self.hf_count("files"), "unaligned files all recorded")
        self.assertEqual(
            0, self.hf_scalar("select count(*) from files where digest is null"),
            "unaligned files all digested")
