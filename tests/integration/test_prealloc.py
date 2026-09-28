"""Files with preallocated (UNWRITTEN) extents, as fallocate() makes them.

A read buffer that touched a preallocated extent used to be filled with zeroes
whole, data and all, so two different files got one digest (#273). Nothing
downstream can tell a wrong digest from a file with no duplicate, so each test
compares against the same bytes written out, which is the digest that is right
by definition.
"""

import os
from harness import DuperemoveTest, fiemap_extents

FIEMAP_EXTENT_UNWRITTEN = 0x0800

MiB = 1024 * 1024


class PreallocTest(DuperemoveTest):
    def prealloc(self, relpath, head, gap, length):
        """`head`, then `gap` bytes of hole, then `length` preallocated bytes."""
        p = self.path(relpath)
        with open(p, "wb") as f:
            f.write(head)
            f.flush()
            os.posix_fallocate(f.fileno(), len(head) + gap, length)
        self.sync()
        if not any(flags & FIEMAP_EXTENT_UNWRITTEN
                   for _l, _p, _n, flags in fiemap_extents(p)):
            self.skipTest("no UNWRITTEN extent: the test would prove nothing")
        return p

    def digest(self, name):
        return self.hf_scalar(
            "select hex(digest) from files where filename like ?",
            ("%/" + name,))

    def test_data_next_to_a_preallocated_tail_is_hashed(self):
        a, b = os.urandom(512 * 1024), os.urandom(512 * 1024)
        self.prealloc("tree/a", a, 0, 512 * 1024)
        self.prealloc("tree/b", b, 0, 512 * 1024)
        self.write("tree/c", a + bytes(512 * 1024))   # a's bytes, written out
        self.scan(self.path("tree"))
        self.assertDmOk()
        self.assertNotEqual(self.digest("a"), self.digest("b"),
                            "different bytes, different digests")
        self.assertEqual(self.digest("a"), self.digest("c"),
                         "the digest describes the bytes, not zeroes")

    def test_data_before_a_hole_and_a_preallocated_extent_is_hashed(self):
        """On a hole, the next extent can start far past the buffer: it must
        not decide what the buffer holds."""
        a, b = os.urandom(64 * 1024), os.urandom(64 * 1024)
        gap = 8 * MiB - len(a)
        self.prealloc("tree/a", a, gap, MiB)
        self.prealloc("tree/b", b, gap, MiB)
        self.make_sparse("tree/c", a, gap, bytes(MiB))
        self.scan(self.path("tree"))
        self.assertDmOk()
        self.assertNotEqual(self.digest("a"), self.digest("b"))
        self.assertEqual(self.digest("a"), self.digest("c"))

    def test_a_row_from_an_older_binary_is_rechecked_once(self):
        """An unchanged file is never hashed again, so a hashfile written
        before the fix would keep its wrong digests. A row without
        FILE_UNWRITTEN_CHECKED (0x4) is rehashed if the file has a
        preallocated extent, and only marked otherwise."""
        self.prealloc("tree/a", os.urandom(512 * 1024), 0, 512 * 1024)
        self.mkrand("tree/plain", 256 * 1024)
        self.scan(self.path("tree"))
        self.assertDmOk()
        right = self.digest("a")
        self.assertEqual(0, self.hf_scalar(
            "select count(*) from files where not (flags & 4)"),
            "every file this binary hashes carries the bit")

        # What an older binary leaves behind: no bit, and a digest nobody
        # checks again. The plain file gets one too, to show it is not rehashed.
        wrong = "00" * 16
        self.hf_exec("update files set flags = flags & ~4, "
                     f"digest = x'{wrong}'")
        self.scan(self.path("tree"))
        self.assertDmOk()
        self.assertEqual(right, self.digest("a"), "rehashed")
        self.assertEqual(wrong.upper(), self.digest("plain"),
                         "no preallocated extent: marked, not rehashed")
        self.assertEqual(0, self.hf_scalar(
            "select count(*) from files where not (flags & 4)"),
            "both rows are marked, so the next run asks nothing")
