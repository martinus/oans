"""`--fdupes` mode: read groups of duplicate paths on stdin and dedupe each.

Regression for upstream #389: two hard links in the same fdupes group share an
inode, which collided on the inode-keyed filerec index and aborted. They must
be skipped (they already share storage), and genuine duplicates must still
dedupe. Requires a reflink-capable filesystem.
"""

import os
from harness import DuperemoveTest, requires_reflink


@requires_reflink
class FdupesTest(DuperemoveTest):
    def test_hardlinks_in_group_do_not_crash(self):
        data = os.urandom(200000)
        a = self.write("tree/a", data)
        b = self.write("tree/b", data)
        h1 = self.write("tree/h1", os.urandom(4096))
        h2 = self.path("tree/h2")
        os.link(h1, h2)          # hard link: same inode as h1
        self.sync()

        # One group with a real duplicate pair, one group with two hard links.
        groups = f"{a}\n{b}\n\n{h1}\n{h2}\n\n"
        self.dm("--fdupes", hashfile=False, stdin=groups)

        self.assertEqual(self.rc, 0, "fdupes must not crash on hard links")
        self.assertNotIn("ERROR:", self.out)
        self.sync()
        self.assertShared(a, b, "the genuine duplicate pair still deduped")
