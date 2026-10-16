"""A generation window must not load a whole-file group it cannot dedupe.

GET_DUPLICATE_FILES loads a window's new members plus the group's target. When
the target is the window's only member and its copies sit in other windows, the
window loaded a group of one, and push_results() printed "Skipping extent -
insufficient duplicates (1)" for it at default verbosity: on a first scan of a
large tree, once per group, thousands of lines in a timer's journal. The loader
now drops such a group from the window, and the per-group line is --debug only,
with one -v summary line if any group is still skipped.

One generation and one pass per file (-B 1, DUPEREMOVE_FILES_PER_PASS=1) puts
every copy in a window of its own, so every group's target is alone in one.
Requires a reflink-capable fs.
"""

import os
from harness import DuperemoveTest, requires_reflink, files_share

ENV = {"DUPEREMOVE_FILES_PER_PASS": "1"}
GROUPS = 5
COPIES = 3


@requires_reflink
class SingleMemberGroupTest(DuperemoveTest):

    def make_tree(self):
        groups = []
        for g in range(GROUPS):
            data = os.urandom(256 * 1024)
            groups.append([self.write(f"tree/g{g}_c{c}", data)
                           for c in range(COPIES)])
        self.sync()
        return groups

    def run_dedupe(self, *extra):
        out = self.dm("-rd", "-B", "1", *extra, self.path("tree"),
                      env=ENV, quiet=False, text=False)
        self.out = out.decode(errors="replace")
        self.assertDmOk()
        return out

    def assertAllDeduped(self, groups):
        for copies in groups:
            for other in copies[1:]:
                self.assertTrue(files_share(copies[0], other),
                                f"{os.path.basename(other)} deduped")

    def test_default_verbosity_prints_no_per_group_line(self):
        groups = self.make_tree()
        out = self.run_dedupe()
        self.assertEqual(out.count(b"insufficient duplicates"), 0,
                         "per-group skip line at default verbosity")
        self.assertAllDeduped(groups)

    def test_the_loader_loads_no_group_of_one(self):
        # Under -v a skipped group would be counted in one summary line; the
        # loader fix means there is nothing to count.
        groups = self.make_tree()
        out = self.run_dedupe("-v")
        self.assertEqual(out.count(b"insufficient duplicates"), 0,
                         "per-group skip line under -v")
        self.assertEqual(out.count(b"groups skipped"), 0,
                         "a window loaded a group of one")
        self.assertAllDeduped(groups)
