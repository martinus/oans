"""Extent-level dedupe: files that are not whole-file duplicates but share a
region. This drives the extent pass end to end - including fiemap_scan_extent()
and fiemap_count_shared(), which the whole-file dedupe tests never reach - and
checks it shares the region, preserves data, and is idempotent.

(It cannot pin fiemap_scan_extent()'s exact post-dedupe offset: that value is
only a hint for the "already deduped" check, and the kernel byte-verifies every
dedupe, so a wrong hint changes neither data nor sharing. This guards the path
against crashes/errors and against sharing/data regressions.)

A punched hole separates the unique head from the shared tail, which is what
puts the tail in an extent of its own for the extent pass to match. A hole is a
property of the extent map rather than of how one filesystem allocates, so
test_shared_tail_is_deduped runs anywhere reflink does.

It used to force that boundary with an fsync, and this file used to explain that
only copy-on-write can produce one - "XFS overwrites in place and keeps the file
as one extent, so it cannot reproduce the partially-shared layout". That was
wrong twice: a hole owes nothing to copy-on-write, and XFS does fragment a file
written around unmapped regions (#242, with filefrag and xfs_bmap output).
test_einval.py has been building this very layout and passing on the XFS leg all
along, which is the evidence the claim was never checked.

The second case stays btrfs-only for a reason the first no longer shares: it
reflinks a file and rewrites the head in place, and it is copy-on-write that
makes such a rewrite replace the head while leaving the tail extents untouched.
XFS reflink is expected to do the same, but that is unverified here.
"""

import os
from harness import DuperemoveTest, requires_btrfs, requires_reflink

MiB = 1 << 20

# Between the unique head and the shared tail: what puts the tail in an extent
# of its own. Block-aligned, and generous, which costs nothing - it is a hole.
HOLE = MiB


class _ExtentFixtures:
    """Setup shared by both cases below.

    A plain mixin rather than a shared TestCase base: unittest collects every
    TestCase subclass, so a base holding tests as well as helpers would run
    those tests again for each case that inherits from it.
    """

    def _mkfile(self, rel, head, tail):
        """head, a punched hole, then the shared tail."""
        return self.make_sparse(rel, head, HOLE, tail)

    def _layout(self, **files):
        """Physical extents per file, for a failure message worth reading."""
        from harness import phys_extents
        return "\n  " + "\n  ".join(
            f"{name}: {sorted(phys_extents(p))}" for name, p in files.items())


@requires_reflink
class ExtentDedupeTest(_ExtentFixtures, DuperemoveTest):
    # Extent-layout sensitive: see DuperemoveTest.serial. The hole makes the
    # boundary itself deterministic, but the assertions still read a map that
    # concurrent writeback moves around, so this stays held back.
    serial = True

    def test_shared_tail_is_deduped(self):
        # Distinct heads, identical tail -> not whole-file dupes, so only the
        # extent pass can share the tail.
        tail = os.urandom(4 * MiB)
        a = self._mkfile("tree/a", os.urandom(64 * 1024), tail)
        b = self._mkfile("tree/b", os.urandom(64 * 1024), tail)
        self.sync()

        self.assertNotShared(a, b, "independent before dedupe")
        before = self.tree_digest(self.path("tree"))

        self.dedupe(self.path("tree"))
        self.assertDmOk()
        self.sync()

        self.assertShared(a, b, "shared tail deduped via the extent pass")
        self.assertEqual(before, self.tree_digest(self.path("tree")),
                         "dedupe preserved file contents")

        # Extent dedupe is idempotent: a second pass adds no new sharing.
        self.dedupe(self.path("tree"))
        self.assertDmOk()
        self.assertNoNewSharing()


@requires_btrfs
class ExtentDedupeCowTest(_ExtentFixtures, DuperemoveTest):
    # Extent-layout sensitive: see DuperemoveTest.serial.
    serial = True

    def _reflink_new_head(self, src_rel, dst_rel, head_len):
        """Reflink src, then rewrite its head in place.

        Copy-on-write replaces only the head blocks, so the copy keeps the
        seed's tail extents byte for byte - a physical class of two that does
        not depend on how writeback laid anything out.
        """
        p = self.reflink(src_rel, dst_rel)
        with open(p, "r+b") as f:
            f.write(os.urandom(head_len))
        return p

    def test_group_on_two_physical_extents_converges_in_one_run(self):
        """Every member lands on the target, not just one per physical extent.

        clean_deduped() used to cull pairwise, keeping one survivor per distinct
        physical offset. Only that survivor is repointed by the ioctl, so the
        rest of its class stayed on the old extent, which was therefore never
        freed - one copy moved per run, nothing reclaimed until the last one
        landed, which on a large tree never happened (#186).

        Runs without a hashfile so each invocation reconsiders the whole tree:
        with one, the incremental dedupe_seq watermark would skip the unchanged
        groups and the second run would be a no-op for the wrong reason.

        The two classes are built with reflinks rather than by deduping the
        subtrees first. A reflink copies the extent map exactly, so each pair
        provably lands in one group; writing four files independently and
        hoping writeback splits all four tails the same way does not survive a
        different allocator (it failed on CI's fresh 2 GiB image while passing
        on a large aged filesystem). Only a1-vs-b1 still relies on two
        head+hole+tail writes matching, and a punched hole puts that boundary
        in the extent map by construction rather than leaving it to writeback.
        """
        head = 64 * 1024        # block-aligned, so rewriting it spares the tail
        tail = os.urandom(4 * MiB)
        a1 = self._mkfile("tree/a/1", os.urandom(head), tail)
        b1 = self._mkfile("tree/b/1", os.urandom(head), tail)
        self.sync()
        # Same tail extents as the seed, different head - so same size but a
        # different file, which keeps all four out of the whole-file pass.
        a2 = self._reflink_new_head("tree/a/1", "tree/a/2", head)
        b2 = self._reflink_new_head("tree/b/1", "tree/b/2", head)
        self.sync()

        self.assertShared(a1, a2, "a/* share one physical copy of the tail")
        self.assertShared(b1, b2, "b/* share the other")
        self.assertNotShared(a1, b1, "the two copies are still independent")

        before = self.tree_digest(self.path("tree"))
        self.dm("-rd", self.path("tree"), hashfile=False)
        self.assertDmOk()
        self.sync()

        # One run has to collapse all four onto one extent. Whichever class the
        # target comes from, the old cull left the other class's non-survivor
        # behind, so check every member against the same reference.
        for other, name in ((a2, "a/2"), (b1, "b/1"), (b2, "b/2")):
            self.assertShared(a1, other,
                              f"{name} still on its old extent after one run"
                              + self._layout(a1=a1, a2=a2, b1=b1, b2=b2))
        self.assertEqual(before, self.tree_digest(self.path("tree")),
                         "dedupe preserved file contents")

        # ... so there is nothing left for a second run to do.
        self.dm("-rd", self.path("tree"), hashfile=False)
        self.assertDmOk()
        self.assertNoNewSharing()
        self.assertReclaimedNothing("second run should find the group settled")
