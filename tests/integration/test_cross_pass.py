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
