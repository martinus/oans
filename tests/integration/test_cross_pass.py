"""Dedupe splits work into generation passes; a group whose copies span many
passes used to be reloaded and re-checked in each one. The loaders now bring
only the copies new in a pass plus one already-deduped representative as the
target. This must (a) still converge every copy onto one physical extent, and
(b) not re-dedupe on a second run. Forced via DUPEREMOVE_FILES_PER_PASS so a
small dataset spans many passes. Requires a reflink-capable fs.
"""

import glob
import os
from harness import DuperemoveTest, requires_reflink, phys_extents, files_share


@requires_reflink
class CrossPassTest(DuperemoveTest):
    # The new test below asserts on physical extent layout.
    serial = True
    # tiny passes + small batchsize => the group appears in many passes
    ENV = {"DUPEREMOVE_FILES_PER_PASS": "8"}
    BOPT = ("-B", "4")

    def test_many_copies_converge_across_passes(self):
        data = os.urandom(96 * 1024)
        paths = [self.write(f"tree/c{i:03d}", data) for i in range(120)]
        self.sync()

        self.dm("-rd", *self.BOPT, self.path("tree"), env=self.ENV)
        self.assertDmOk()
        self.sync()

        # Every copy must end on the SAME single physical extent - a per-pass
        # target would leave one cluster per pass.
        allphys = set()
        for p in paths:
            allphys |= phys_extents(p)
        self.assertEqual(len(allphys), 1,
                         f"all 120 copies converge to one extent, got {len(allphys)}")

        # Data intact.
        for p in (paths[0], paths[60], paths[-1]):
            self.assertEqual(open(p, "rb").read(), data, "content preserved")

    def test_partial_dups_across_passes(self):
        # Independent identical pairs whose members land in different passes.
        pairs = []
        for i in range(40):
            d = os.urandom(20000 + i)
            a = self.write(f"tree/a{i:02d}", d)
            b = self.write(f"tree/z{i:02d}", d)
            pairs.append((a, b))
        self.sync()

        self.dm("-rd", *self.BOPT, self.path("tree"), env=self.ENV)
        self.assertDmOk()
        self.sync()
        for a, b in pairs:
            self.assertTrue(files_share(a, b),
                            f"{os.path.basename(a)} pair deduped")

    def test_noop_rerun_across_passes(self):
        data = os.urandom(96 * 1024)
        for i in range(120):
            self.write(f"tree/c{i:03d}", data)
        self.sync()
        self.dm("-rd", *self.BOPT, self.path("tree"), env=self.ENV)
        self.assertDmOk()
        self.sync()

        self.dm("-rd", *self.BOPT, self.path("tree"), env=self.ENV)
        self.assertDmOk()
        self.assertNoNewSharing()

    def test_target_is_the_least_fragmented_member_not_the_first(self):
        """The least-fragmented copy is still preferred as the target.

        Target election moved out of the worker's live fiemap and into the
        loader, so that every generation window reaches the same answer (#197).
        This guards the half of that change which is easy to lose silently: the
        preference itself. Built so the two candidate rules disagree - the
        lowest-id copy is deliberately the most fragmented - so a target chosen
        by id alone would land somewhere this assertion can see.

        It does *not* reproduce #197: three copies occupy a single window, and
        the bug needs a group spread across two windows that are in flight
        together. test_many_copies_converge_across_passes covers that, at the
        cost of only failing when the race actually happens.
        """
        data = os.urandom(4 << 20)

        # c000 first, and fragmented: many flushed writes force extent splits.
        with open(self.path("tree/c000"), "wb") as f:
            for off in range(0, len(data), 64 << 10):
                f.write(data[off:off + (64 << 10)])
                f.flush()
                os.fsync(f.fileno())
        # ... then two contiguous copies, so id order and extent order differ.
        for name in ("c001", "c002"):
            self.write(f"tree/{name}", data)
        self.sync()

        frag = len(phys_extents(self.path("tree/c000")))
        tidy = len(phys_extents(self.path("tree/c001")))
        self.assertGreater(frag, tidy,
                           "setup failed: c000 was meant to be the fragmented one")
        target_phys = phys_extents(self.path("tree/c001"))

        self.dm("-rd", *self.BOPT, self.path("tree"), env=self.ENV)
        self.assertDmOk()
        self.sync()

        landed = phys_extents(self.path("tree/c000"))
        self.assertTrue(landed & target_phys,
                        "copies converged onto the fragmented lowest-id copy "
                        f"instead of the tidy one: {sorted(landed)} vs "
                        f"{sorted(target_phys)}")
        self.assertEqual(1, len({p for n in ("c000", "c001", "c002")
                                 for p in phys_extents(self.path(f"tree/{n}"))}),
                         "all three copies should share one extent")
