"""An extent's digest covers the extent, not the hole in front of it.

The scan skips only whole hash blocks (128 KiB) of hole; a hole that does not
fill them is read, and comes back as zeroes. Those zeroes used to be hashed
into the next extent's digest while its row still said it starts at
fe_logical and runs fe_length, so the same extent behind holes of different
sizes got a different digest each time and the extent pass never matched it -
the layout of VM images, databases and torrents. Nothing said so.
"""

import os
from harness import DuperemoveTest, fiemap_extents, requires_reflink

KiB = 1 << 10

TAIL = 64 * KiB
# 128 KiB fills a hash block, so it was skipped and always hashed right.
HOLES = {"a": 64 * KiB, "b": 32 * KiB, "c": 128 * KiB}


@requires_reflink
class HoleExtentTest(DuperemoveTest):
    # Asserts on which physical extent each tail ended up in.
    serial = True

    def build(self):
        """The same tail behind three holes, one file each. The files differ
        in size, so only the extent pass can match the tails."""
        tail = os.urandom(TAIL)
        for name, hole in HOLES.items():
            self.make_sparse(f"tree/{name}", b"", hole, tail)
        self.sync()
        for name, hole in HOLES.items():
            if self.tail_record(name) is None:
                self.skipTest("the tail is not one extent behind a hole here")
        return self.path("tree")

    def tail_record(self, name):
        """(loff, poff, len) of the one extent holding the tail, or None."""
        hole = HOLES[name]
        recs = [(l, p, n) for l, p, n, _fl in
                fiemap_extents(self.path("tree", name))]
        if len(recs) == 1 and recs[0][0] == hole and recs[0][2] == TAIL:
            return recs[0]
        return None

    def tail_digest(self, name):
        return self.hf_scalar(
            "select hex(e.digest) from extents e join files f "
            "on f.id = e.fileid where f.filename like ? and e.loff = ?",
            ("%/" + name, HOLES[name]))

    def tail_digests(self):
        return {name: self.tail_digest(name) for name in HOLES}

    def test_one_extent_behind_any_hole_has_one_digest(self):
        self.scan(self.build())
        self.assertDmOk()
        digests = self.tail_digests()
        self.assertNotIn(None, digests.values(), "every tail has its row")
        self.assertEqual(1, len(set(digests.values())),
                         f"one extent, one digest: {digests}")

    def test_the_tails_are_deduped(self):
        tree = self.build()
        before = self.tree_digest(tree)
        self.dedupe(tree)
        self.assertDmOk()
        self.sync()
        poffs = {name: self.tail_record(name)[1] for name in HOLES}
        self.assertEqual(1, len(set(poffs.values())),
                         f"the three tails share one extent: {poffs}")
        self.assertEqual(before, self.tree_digest(tree))

    def test_a_row_from_an_older_binary_is_rechecked_once(self):
        """An unchanged file is never hashed again, so a hashfile written
        before the fix would keep its wrong digests. A row without
        FILE_HOLE_EXTENT_CHECKED (0x10) is rehashed if a hole the scan reads
        comes before a data extent, and only marked otherwise."""
        self.scan(self.build())
        self.assertDmOk()
        right = self.tail_digest("a")
        self.assertEqual(0, self.hf_scalar(
            "select count(*) from files where not (flags & 16)"),
            "every file this binary hashes carries the bit")

        # What an older binary leaves behind: no bit, and extent digests
        # nobody checks again. c's hole fills a hash block, so it is the one
        # file that was always hashed right and is not rehashed.
        wrong = "00" * 16
        self.hf_exec("update files set flags = flags & ~16")
        self.hf_exec(f"update extents set digest = x'{wrong}'")
        self.scan(self.path("tree"))
        self.assertDmOk()
        self.assertEqual({"a": right, "b": right, "c": wrong.upper()},
                         self.tail_digests(),
                         "a and b rehashed, c marked and left alone")
        self.assertEqual(0, self.hf_scalar(
            "select count(*) from files where not (flags & 16)"),
            "every row is marked, so the next run asks nothing")
