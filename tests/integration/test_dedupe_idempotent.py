"""A second dedupe of an unchanged tree must be a no-op.

Every destination oans hands the kernel a second time is a full byte-compare of
the range for nothing, and the kernel reports those bytes as deduped, so a run
that frees no space can still claim to have reclaimed gigabytes (#186). What
keeps that from happening is the already-shared skip, which has to recognise
the layouts dedupe itself leaves behind: a tail split on a block boundary, a
file smaller than one block, a sparse file whose extent map stops well before
the end of the range.

These run without a hashfile so each invocation reconsiders the whole tree.
With one, the incremental dedupe_seq watermark would skip the unchanged groups
and every second run would pass for the wrong reason.
"""

import os
from harness import DuperemoveTest, requires_reflink

KiB = 1 << 10


@requires_reflink
class DedupeIdempotentTest(DuperemoveTest):
    # Every case here turns on the physical layout dedupe leaves behind - a
    # split tail, a hole map, a sub-block extent - so it belongs in the
    # one-at-a-time pass. See DuperemoveTest.serial: concurrent I/O perturbs
    # btrfs writeback, and CI caught exactly that (a valgrind shard failed
    # test_tail_block_split_off while the plain btrfs legs passed).
    serial = True

    def assertRunFindsNothingToDo(self, tree, why):
        """One dedupe over `tree` that must free nothing and say why."""
        self.dm("-rd", tree, hashfile=False, quiet=False)
        self.assertDmOk()
        self.assertReclaimedNothing(why)
        # Distinguishes "recognised as shared" from "no group at all" - the
        # latter would make every assertion above pass without testing
        # anything. On failure the run's own summary says which it was.
        self.assertGreater(self.already_shared(), 0,
                           "expected the already-shared skip to fire; oans said:"
                           f"\n{self.out.strip()}")

    def assertSecondRunIsNoop(self, tree):
        """Dedupe twice; the second run must reclaim nothing."""
        self.dm("-rd", tree, hashfile=False)
        self.assertDmOk("first dedupe")
        self.sync()
        self.assertRunFindsNothingToDo(tree, "the tree is already deduped")

    def test_block_aligned_files(self):
        self.mkdup("tree/a", "tree/b", 64 * KiB)
        self.sync()
        self.assertSecondRunIsNoop(self.path("tree"))

    def test_unaligned_tail(self):
        """A size that is not a whole number of blocks.

        The kernel will not share the trailing partial block, so the copy keeps
        its own tail record forever. Comparing that tail makes the skip
        unsatisfiable and the whole file is resubmitted on every run.
        """
        self.mkdup("tree/a", "tree/b", 64 * KiB + 1234)
        self.sync()
        self.assertSecondRunIsNoop(self.path("tree"))

    def test_file_smaller_than_a_block(self):
        """The whole file is one partial block: there is no aligned part to
        compare, so the skip has to fall back to comparing it whole.

        Sized above btrfs's max_inline (2048 by default) on purpose: an inline
        extent has no physical location, the kernel declines to dedupe it, and
        the run comes out at zero however the skip behaves - which would make
        this pass without testing anything.
        """
        self.mkdup("tree/a", "tree/b", 4 * KiB - 96)
        self.sync()
        self.assertSecondRunIsNoop(self.path("tree"))

    def test_tail_block_split_off(self):
        """Every full block shared, the trailing partial block not.

        This is the layout dedupe leaves on a file whose size is not a whole
        number of blocks, because the kernel rounds the length down and never
        shares that last block. If the already-shared check insists on it, the
        file can never pass and its whole length is byte-compared again on
        every run - which is where most of #186's phantom "reclaimed" came
        from. Built by hand here (reflink, then rewrite the tail with the same
        bytes so copy-on-write splits it off) rather than by deduping, so the
        test does not depend on how the kernel treats an unaligned tail.

        Asserted on the *first* run: there is nothing here left to share, and
        recognising that without byte-comparing the whole file is the point.
        """
        aligned = 128 * KiB
        data = os.urandom(aligned + 2000)
        self.write("tree/a", data)
        self.sync()
        b = self.reflink("tree/a", "tree/b")
        with open(b, "r+b") as f:
            f.seek(aligned)
            f.write(data[aligned:])
        self.sync()

        self.assertRunFindsNothingToDo(self.path("tree"),
                                       "nothing here is left to share")

    def test_sparse_twins(self):
        """fiemap omits holes, so the map stops short of the range end. Two
        files with the same hole layout share it; that must not read as a
        difference."""
        head, tail = os.urandom(8 * KiB), os.urandom(8 * KiB)
        self.make_sparse("tree/a", head, 256 * KiB, tail)
        self.make_sparse("tree/b", head, 256 * KiB, tail)
        self.sync()
        self.assertSecondRunIsNoop(self.path("tree"))

    def test_trailing_hole(self):
        """Same, with the hole at the end - the map simply runs out."""
        data = os.urandom(8 * KiB)
        self.make_trailing_hole("tree/a", data, 256 * KiB)
        self.make_trailing_hole("tree/b", data, 256 * KiB)
        self.sync()
        self.assertSecondRunIsNoop(self.path("tree"))
