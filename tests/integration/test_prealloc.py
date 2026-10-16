"""Files with preallocated (UNWRITTEN) extents, as fallocate() makes them.

A read buffer that touched a preallocated extent used to be filled with zeroes
whole, data and all, so two different files got one digest (#273). Nothing
downstream can tell a wrong digest from a file with no duplicate, so each test
compares against the same bytes written out, which is the digest that is right
by definition.
"""

import os
from harness import (DuperemoveTest, fiemap_extents, preallocate_past_eof,
                     requires_reflink)

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


@requires_reflink
class PreallocPastEofTest(DuperemoveTest):
    """The extent holding EOF runs to the end of its block, past the file size.

    That overshoot was allowed for only when the extent was FIEMAP_EXTENT_LAST.
    A preallocated extent past EOF (`fallocate -n`, as journald and download
    clients do) follows it, so it is not LAST: the scan never reached the
    extent's end and stored no row for it, and the extent pass could never
    dedupe it. Nothing said so.
    """
    # Asserts on the physical layout of the shared tail.
    serial = True

    TAIL = 10000            # not a block multiple: the extent runs past EOF

    def preallocate_after(self, p, size):
        with open(p, "r+b") as f:
            f.flush()
            os.fsync(f.fileno())
            preallocate_past_eof(f.fileno(), size + 16 * 1024, MiB)
        self.sync()
        recs = fiemap_extents(p)
        if not any(fl & FIEMAP_EXTENT_UNWRITTEN for _l, _p, _n, fl in recs):
            self.skipTest("no UNWRITTEN extent: the test would prove nothing")
        tail = [(l, n) for l, _p, n, fl in recs
                if not fl & FIEMAP_EXTENT_UNWRITTEN and l < size < l + n]
        if not tail:
            self.skipTest("no data extent runs past EOF here")
        return tail[0]

    def test_the_extent_holding_eof_gets_its_row(self):
        p = self.mkrand("tree/a", self.TAIL)
        tail = self.preallocate_after(p, self.TAIL)
        self.scan(self.path("tree"))
        self.assertDmOk()
        rows = set(self.hf_query("select loff, len from extents"))
        self.assertIn(tail, rows, "the data extent before the preallocated "
                      "one is stored")

    def test_a_row_from_an_older_binary_gets_the_missing_extent(self):
        """An unchanged file is never hashed again, so a row an older binary
        wrote would lack the extent for good. A row without
        FILE_EOF_EXTENT_CHECKED (0x8) is rehashed if its EOF extent has a
        record after it, and only marked otherwise."""
        p = self.mkrand("tree/a", self.TAIL)
        tail = self.preallocate_after(p, self.TAIL)
        self.mkrand("tree/plain", self.TAIL)
        self.scan(self.path("tree"))
        self.assertDmOk()
        self.assertEqual(0, self.hf_scalar(
            "select count(*) from files where not (flags & 8)"),
            "every file this binary hashes carries the bit")

        # What 1.13 leaves behind: the #273 bit, no extent row, no 0x8.
        self.hf_exec("delete from extents where loff = ? and fileid = "
                     "(select id from files where filename like '%/a')",
                     (tail[0],))
        self.hf_exec("update files set flags = flags & ~8")
        # The plain file gets a digest nobody checks again, to show it is not
        # rehashed.
        plain = ("select hex(e.digest) from extents e join files f on "
                 "f.id = e.fileid where f.filename like '%/plain'")
        self.hf_exec(f"update extents set digest = x'00' "
                     f"where hex(digest) in ({plain})")
        self.scan(self.path("tree"))
        self.assertDmOk()
        self.assertIn(tail, set(self.hf_query("select loff, len from extents")),
                      "rehashed")
        self.assertEqual([("00",)], self.hf_query(plain),
                         "no record after its EOF extent: marked, not rehashed")
        self.assertEqual(0, self.hf_scalar(
            "select count(*) from files where not (flags & 8)"),
            "both rows are marked, so the next run asks nothing")

    def test_two_such_tails_are_deduped(self):
        """Different heads, so only the extent pass can share the tails."""
        tail = os.urandom(self.TAIL)
        head, hole = 64 * 1024, 64 * 1024
        size = head + hole + self.TAIL
        a = self.make_sparse("tree/a", os.urandom(head), hole, tail)
        b = self.make_sparse("tree/b", os.urandom(head), hole, tail)
        self.preallocate_after(a, size)
        self.preallocate_after(b, size)
        before = self.tree_digest(self.path("tree"))
        self.assertNotShared(a, b)
        self.dedupe(self.path("tree"))
        self.assertDmOk()
        self.sync()
        # Not assertShared(): btrfs may split the tail, and the piece wholly
        # inside the file was deduped before the fix as well.
        self.assertEqual(self.eof_physical(a, size),
                         self.eof_physical(b, size),
                         "the extents holding EOF were deduped")
        self.assertEqual(before, self.tree_digest(self.path("tree")))

    def eof_physical(self, p, size):
        return [ph for l, ph, n, _fl in fiemap_extents(p)
                if l < size <= l + n]
