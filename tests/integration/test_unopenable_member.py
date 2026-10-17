"""A group member the dedupe phase cannot open is counted as not deduped.

A deleted member (ENOENT) is pruned from the hashfile and is not a failure. Any
other open error - a permission change, EIO - leaves a file that is still there
and still a duplicate, and the summary's "Not deduped" line has to say so; it
used to count nothing, and a -q run reported a clean result.
"""

import os
import re
from harness import DuperemoveTest, requires_reflink

MiB = 1 << 20


@requires_reflink
class UnopenableMemberTest(DuperemoveTest):

    def test_an_unopenable_member_is_counted_as_failed(self):
        if os.geteuid() == 0:
            self.skipTest("root opens a mode-000 file anyway")
        os.makedirs(self.path("tree"))
        data = os.urandom(MiB)
        a, b, c = (self.write(f"tree/{n}", data) for n in "abc")
        self.sync()

        self.dm("-r", self.path("tree"))
        self.assertDmOk()
        os.chmod(c, 0)

        # Dedupe from the hashfile as recorded: scan an unrelated empty dir.
        empty = self.path("empty")
        os.makedirs(empty)
        self.dm("-d", "-r", empty)
        self.assertEqual(self.rc, 0, self.out)

        m = re.search(r"Not deduped: (.*) \(rerun", self.out)
        self.assertIsNotNone(m, f"no 'Not deduped' line:\n{self.out}")
        self.assertIn("1 failed", m.group(1), self.out)
        self.assertShared(a, b, "the members that opened still dedupe")
