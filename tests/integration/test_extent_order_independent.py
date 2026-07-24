"""Order-independence of the extent pass vs the whole-file pass.

A file that is a whole-file duplicate has its extent rows removed by the
whole-file pass before the extent pass loads (historically a *temporal*
exclusion). Stage 2 makes the extent loader exclude whole-file-dup members
*statically* (a query change) so the extent load no longer depends on the
whole-file pass having finished first - the prerequisite for pipelining the two
passes. The end state must be identical either way, which is what these tests
pin. Written against the pre-change build first, they must keep passing after.

Requires btrfs: the setup relies on an fsync-forced extent boundary so the
shared tail is its own extent the extent pass can match (see test_extent_dedupe).
"""

import os
from harness import DuperemoveTest, requires_btrfs

MiB = 1 << 20


@requires_btrfs
class ExtentOrderIndependentTest(DuperemoveTest):
    def _mkfile(self, rel, head, tail):
        """head, an fsync to force an extent boundary, then the shared tail."""
        p = self.path(rel)
        with open(p, "wb") as f:
            f.write(head)
            f.flush()
            os.fsync(f.fileno())
            f.write(tail)
        return p

    def test_extent_group_survives_removal_of_whole_file_members(self):
        # A,B are whole-file identical (same head H1 + tail T); C,D share only
        # the tail T (distinct heads). The tail extent group is {A,B,C,D}.
        # The whole-file pass dedupes {A,B} and drops their extent rows, so the
        # extent pass must still dedupe the remaining {C,D}.
        h1, h2, h3 = (os.urandom(64 * 1024) for _ in range(3))
        tail = os.urandom(4 * MiB)
        a = self._mkfile("tree/a", h1, tail)
        b = self._mkfile("tree/b", h1, tail)
        c = self._mkfile("tree/c", h2, tail)
        d = self._mkfile("tree/d", h3, tail)
        self.sync()
        before = self.tree_digest(self.path("tree"))

        self.dedupe(self.path("tree"))
        self.assertDmOk()
        self.sync()

        self.assertShared(a, b, "whole-file pair A,B deduped")
        self.assertShared(c, d, "extent pair C,D deduped despite A,B removal")
        self.assertEqual(before, self.tree_digest(self.path("tree")),
                         "dedupe preserved file contents")

        # Idempotent.
        self.dedupe(self.path("tree"))
        self.assertDmOk()
        self.assertNoNewSharing()

    def test_extent_group_dropped_when_only_whole_file_members_remain(self):
        # A,B whole-file identical + a single extent-sharer C (tail T). The tail
        # group is {A,B,C}; once A,B are excluded only C is left, so the extent
        # pass has nothing to dedupe and C stays independent.
        h1, h2 = os.urandom(64 * 1024), os.urandom(64 * 1024)
        tail = os.urandom(4 * MiB)
        a = self._mkfile("tree/a", h1, tail)
        b = self._mkfile("tree/b", h1, tail)
        c = self._mkfile("tree/c", h2, tail)
        self.sync()
        before = self.tree_digest(self.path("tree"))

        self.dedupe(self.path("tree"))
        self.assertDmOk()
        self.sync()

        self.assertShared(a, b, "whole-file pair A,B deduped")
        self.assertNotShared(c, a, "lone extent member C left independent")
        self.assertNotShared(c, b, "lone extent member C left independent")
        self.assertEqual(before, self.tree_digest(self.path("tree")),
                         "dedupe preserved file contents")
