"""End-to-end data integrity: a mixed, realistic tree survives a full dedupe run
byte-for-byte, while duplicates actually get shared. The catch-all that would
flag any dedupe path that corrupts, truncates, or loses data."""

import os
from harness import DuperemoveTest, requires_reflink

MiB = 1 << 20


@requires_reflink
class IntegrityTest(DuperemoveTest):
    def test_mixed_tree_integrity(self):
        t = self.path("tree")

        # Unique files of assorted (often unaligned) sizes.
        self.mkrand("tree/uniq/u1", 300000 + 11)
        self.mkrand("tree/uniq/u2", 150000 + 4095)
        self.mkrand("tree/uniq/u3", 4096)

        # A duplicate pair and a duplicate triple (dedupe targets).
        self.mkdup("tree/dup/pairA", "tree/dup/pairB", MiB)
        tri = self.mkrand("tree/dup/tri1", 512 * 1024 + 123)
        data = open(tri, "rb").read()
        self.write("tree/dup/tri2", data)
        self.write("tree/dup/tri3", data)

        # Hardlinks to a unique inode (must not break scanning or dedupe).
        self.mkrand("tree/links/orig", 90000)
        self.hardlink("tree/links/orig", "tree/links/hardlink")

        # A sparse file and an identical, independent sparse copy.
        head, tail = os.urandom(65536), os.urandom(50000)
        self.make_sparse("tree/sparse/s1", head, 262144, tail)
        self.make_sparse("tree/sparse/s2", head, 262144, tail)

        self.sync()
        before = self.tree_digest(t)
        nfiles = sum(len(f) for _r, _d, f in os.walk(t))

        self.dedupe(t)
        self.assertDmOk()
        self.sync()

        # Nothing corrupted or lost.
        self.assertEqual(before, self.tree_digest(t), "every file byte-identical after dedupe")
        self.assertEqual(nfiles, sum(len(f) for _r, _d, f in os.walk(t)), "no files vanished")

        # Duplicates actually got shared.
        self.assertShared(self.path("tree/dup/pairA"), self.path("tree/dup/pairB"), "pair shared")
        self.assertShared(self.path("tree/dup/tri1"), self.path("tree/dup/tri2"), "tri 1<->2")
        self.assertShared(self.path("tree/dup/tri1"), self.path("tree/dup/tri3"), "tri 1<->3")
        self.assertShared(self.path("tree/sparse/s1"), self.path("tree/sparse/s2"), "sparse")

        # Unique files were left independent.
        self.assertNotShared(self.path("tree/uniq/u1"), self.path("tree/uniq/u2"),
                             "unique files not shared")

    def test_second_run_stable(self):
        t = self.path("tree")
        self.mkdup("tree/a", "tree/b", MiB)
        self.mkrand("tree/c", MiB)
        self.sync()

        self.dedupe(t); self.assertDmOk(); self.sync()
        after_first = self.tree_digest(t)

        self.dedupe(t); self.assertDmOk(); self.sync()
        self.assertEqual(after_first, self.tree_digest(t), "second full run preserves data")
        self.assertNoNewSharing()
