"""The two properties a dedupe run's summary cannot show you.

Both #186 and #187 shipped for months because the suite checked what oans
*said* and which extents it *shared*, but never whether a run finished the job
or whether the space it claimed to free was freed. The two bugs concealed each
other: the inflated figure made a run that freed nothing look like one that
worked.

So this file asserts against ground truth read back from the kernel:

  - convergence - a second run over an unchanged tree must free nothing, over
    every layout dedupe has to cope with, all sharing one run. #186 only
    reproduced at that scale: every subdirectory converged on its own while
    their union did not.
  - honesty - the reported figure against the drop in physical storage the
    tree references, measured from fiemap rather than restated from anything
    oans computed.
"""

import os
from harness import (DuperemoveTest, phys_extents, physical_footprint,
                     requires_reflink)

KiB = 1 << 10
MiB = 1 << 20


@requires_reflink
class ConvergenceTest(DuperemoveTest):
    # Every layout here is built for its physical shape; see
    # DuperemoveTest.serial.
    serial = True

    # -- layout builders ---------------------------------------------------
    #
    # Each makes one duplicate group of a different shape. Data is always
    # os.urandom(): incompressible, so physical_footprint() measures real disk
    # ranges and a compressed extent's single address cannot blur the result.

    def _independent(self, d, copies=2, size=MiB):
        """The ordinary case: N copies, each with its own storage."""
        data = os.urandom(size)
        for i in range(copies):
            self.write(f"{d}/f{i}", data)

    def _three_way(self, d):
        self._independent(d, copies=3)

    def _unaligned(self, d):
        """A size that is not a whole number of blocks."""
        self._independent(d, size=64 * KiB + 1234)

    def _sub_block(self, d):
        """Smaller than one block, but over btrfs's max_inline (2048): an
        inline extent has no address and cannot be deduplicated at all."""
        self._independent(d, size=4 * KiB - 96)

    def _sparse(self, d):
        """fiemap omits holes, so the map stops short of the range end."""
        head, tail = os.urandom(8 * KiB), os.urandom(8 * KiB)
        for name in ("a", "b"):
            self.make_sparse(f"{d}/{name}", head, 256 * KiB, tail)

    def _trailing_hole(self, d):
        """Same, with the map simply running out before the range does."""
        data = os.urandom(8 * KiB)
        for name in ("a", "b"):
            self.make_trailing_hole(f"{d}/{name}", data, 256 * KiB)

    def _split_tail(self, d):
        """Every full block shared, the trailing partial block not - what a
        dedupe of an unaligned file leaves behind."""
        aligned = 128 * KiB
        data = os.urandom(aligned + 2000)
        self.write(f"{d}/a", data)
        self.sync()
        b = self.reflink(f"{d}/a", f"{d}/b")
        with open(b, "r+b") as f:
            f.seek(aligned)
            f.write(data[aligned:])

    def _half_shared(self, d):
        """Half the copy already sits on the target's extents (#187)."""
        data = os.urandom(2 * MiB)
        self.write(f"{d}/a", data)
        self.sync()
        b = self.reflink(f"{d}/a", f"{d}/b")
        with open(b, "r+b") as f:
            f.seek(MiB)
            f.write(data[MiB:])

    def _two_physical_classes(self, d):
        """One group over two extents, two members each - the shape that needed
        N runs to converge before #186."""
        data = os.urandom(MiB)
        self.write(f"{d}/a1", data)
        self.write(f"{d}/b1", data)
        self.sync()
        self.reflink(f"{d}/a1", f"{d}/a2")
        self.reflink(f"{d}/b1", f"{d}/b2")

    def _two_extent_classes(self, d):
        """The same two-class shape, but reached through the *extent* pass:
        one shared tail behind four different heads, so the whole-file pass
        cannot claim these. That distinction matters - whole-file groups load
        with no physical offsets and skip the cheap cull entirely, so only this
        version exercises it, and only this version reproduces #186.
        """
        head = 64 * KiB
        tail = os.urandom(4 * MiB)
        for seed in ("a", "b"):
            with open(self.path(f"{d}/{seed}1"), "wb") as f:
                f.write(os.urandom(head))
                f.flush()
                os.fsync(f.fileno())     # force an extent boundary
                f.write(tail)
        self.sync()
        for seed in ("a", "b"):
            copy = self.reflink(f"{d}/{seed}1", f"{d}/{seed}2")
            with open(copy, "r+b") as f:
                f.write(os.urandom(head))

    # Every layout, for the convergence property.
    ALL = ("independent", "three_way", "unaligned", "sub_block", "sparse",
           "trailing_hole", "split_tail", "half_shared",
           "two_physical_classes", "two_extent_classes")
    # Layouts where the reported figure is exactly the storage freed. The two
    # two-class ones are here because of #191: their members already share
    # storage with each other, which used to be credited once per member.
    EXACT = ("independent", "three_way", "sparse", "trailing_hole",
             "half_shared", "two_physical_classes", "two_extent_classes")
    # Already as shared as the kernel can make them, so a run must free nothing
    # and say so - the case an over-reporting figure used to hide.
    NOTHING_TO_FREE = ("split_tail",)
    # Layouts that free whole blocks while the figure counts logical bytes, so
    # it understates by up to one block - the harmless direction, and inherent
    # to Reclaimed being a logical figure.
    BLOCK_ROUNDED = ("unaligned", "sub_block")

    def _build(self, names, root):
        """Build each named layout under its own directory in `root`."""
        for name in names:
            getattr(self, "_" + name)(f"{root}/{name}")
        self.sync()
        return self.path(root)

    # -- the properties ----------------------------------------------------

    def test_second_run_over_every_layout_is_a_noop(self):
        """Convergence, over all the layouts at once.

        Without a hashfile, so each invocation reconsiders the whole tree: the
        incremental watermark would otherwise make the second run a no-op for
        the wrong reason.
        """
        tree = self._build(self.ALL, "tree")
        digest = self.tree_digest(tree)

        self.dm("-rd", tree, hashfile=False)
        self.assertDmOk("first dedupe")
        self.sync()
        settled = physical_footprint(tree)

        self.dm("-rd", tree, hashfile=False, quiet=False)
        self.assertDmOk("second dedupe")
        self.sync()

        self.assertReclaimedNothing("one run has to finish the job")
        self.assertEqual(settled, physical_footprint(tree),
                         "second run moved storage around for nothing")
        self.assertEqual(digest, self.tree_digest(tree),
                         "dedupe preserved file contents")

    def test_reclaimed_matches_storage_freed(self):
        """The reported figure against the storage that stopped being used.

        Measured from fiemap, so it is ground truth rather than a restatement
        of what oans computed - the check neither #186 nor #187 had, which is
        why both could ship. Per layout, so a failure names the shape.

        The two-class layouts matter most here: their members already share
        storage with each other, so crediting each in full claimed twice what
        releasing that one extent frees (#191).
        """
        seen = 0
        for name in self.EXACT + self.BLOCK_ROUNDED + self.NOTHING_TO_FREE:
            with self.subTest(layout=name):
                # Its own root, so each run scans only this layout.
                tree = self._build([name], f"t_{name}")
                digest = self.tree_digest(tree)
                before = physical_footprint(tree)

                self.dedupe(tree)       # a hashfile, so --json can be read
                self.assertDmOk()
                self.sync()

                freed = before - physical_footprint(tree)
                total = self.reclaimed_bytes()
                reported, seen = total - seen, total

                if name in self.NOTHING_TO_FREE:
                    self.assertEqual(0, freed, "expected nothing to free")
                    self.assertEqual(0, reported, "claimed a saving it did not make")
                elif name in self.EXACT:
                    self.assertGreater(freed, 0, "nothing was deduplicated")
                    self.assertEqual(freed, reported,
                                     "reported Reclaimed is not what was freed")
                else:
                    self.assertGreater(freed, 0, "nothing was deduplicated")
                    # Over-claiming is #187; understating by block rounding is
                    # inherent to reporting logical bytes.
                    self.assertLessEqual(reported, freed, "over-claimed")
                    self.assertLess(freed - reported, 64 * KiB,
                                    "shortfall exceeds block rounding")
                self.assertEqual(digest, self.tree_digest(tree),
                                 "dedupe preserved file contents")

    def test_footprint_helper_sees_sharing(self):
        """Guard the measurement itself: a reflink must not add footprint and
        an independent copy must, or a broken helper would make the assertions
        above pass by measuring nothing."""
        data = os.urandom(MiB)
        a = self.write("t/a", data)
        self.sync()
        base = physical_footprint(self.path("t"))
        self.assertGreaterEqual(base, MiB)

        self.reflink("t/a", "t/shared")
        self.sync()
        self.assertEqual(base, physical_footprint(self.path("t")),
                         "a reflink references the same storage")

        b = self.write("t/independent", data)
        self.sync()
        self.assertEqual(base * 2, physical_footprint(self.path("t")),
                         "an independent copy doubles the storage referenced")
        self.assertFalse(phys_extents(a) & phys_extents(b))
