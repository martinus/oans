"""oans --stats reports a summary of a hashfile (folds in the old hashstats)."""

import os
from harness import DuperemoveTest


class StatsTest(DuperemoveTest):
    def test_stats_reports_hashfile_summary(self):
        data = os.urandom(50000)
        for n in ("a", "b", "c"):            # three identical files -> one dup group
            self.write(f"tree/{n}", data)
        self.write("tree/u", os.urandom(7001))   # unique
        self.sync()
        self.dm("-rd", self.path("tree"))        # build the hashfile
        self.assertDmOk()

        out = self.dm("--stats")                 # report on it
        self.assertIn("oans hashfile", out)
        self.assertIn("format", out)
        self.assertIn("whole-file duplicates", out)
        self.assertRegex(out, r"tracked\s+4")
        self.assertRegex(out, r"groups\s+1")
        self.assertIn("duplicated", out)
        # top-groups list: the a/b/c group of 3 shows as "... x 3 ..."
        self.assertIn("top groups", out)
        self.assertRegex(out, r"x 3\b")
        self.assertIn("timing", out)
        self.assertRegex(out, r"load\s+\d")
        self.assertIn("dup analysis", out)

    def test_stats_requires_hashfile(self):
        proc = self.dm("--stats", hashfile=False)
        self.assertNotEqual(0, self.rc)
        self.assertIn("hashfile", proc)
