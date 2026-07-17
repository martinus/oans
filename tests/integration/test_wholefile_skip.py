"""In --dedupe-options=only_whole_files mode a whole-file duplicate must have the
same size, so oans only reads files whose size is shared by another. A file with
a unique size is never opened and keeps a NULL digest. These tests pin that the
skip is correct: real duplicates still dedupe, same-size-different-content files
don't, and a file skipped as unique gets hashed once a same-size file appears.
"""

import os
from harness import DuperemoveTest, requires_reflink, files_share

WF = ("--dedupe-options=only_whole_files",)


@requires_reflink
class WholeFileSkipTest(DuperemoveTest):
    def _digest(self, name):
        return self.hf_scalar(
            f"select digest from files where filename like '%/{name}'")

    def test_unique_size_skipped_dups_deduped(self):
        data = os.urandom(40000)
        a = self.write("tree/a", data)
        b = self.write("tree/b", data)          # whole-file dup of a
        u = self.write("tree/u", os.urandom(5001))   # unique size
        self.sync()
        self.dm("-rd", *WF, self.path("tree"))
        self.assertDmOk()
        self.sync()
        self.assertTrue(files_share(a, b), "whole-file duplicate deduped")
        self.assertIsNotNone(self._digest("a"), "shared-size file was hashed")
        self.assertIsNone(self._digest("u"), "unique-size file left unhashed")

    def test_same_size_different_content_not_deduped(self):
        c1 = self.write("tree/c1", os.urandom(8000))
        c2 = self.write("tree/c2", os.urandom(8000))   # same size, different data
        self.sync()
        self.dm("-rd", *WF, self.path("tree"))
        self.assertDmOk()
        self.sync()
        # Both were read (size collides) but they are not duplicates.
        self.assertIsNotNone(self._digest("c1"))
        self.assertIsNotNone(self._digest("c2"))
        self.assertFalse(files_share(c1, c2), "distinct content not deduped")

    def test_unique_then_collision_gets_deduped(self):
        data = os.urandom(33333)
        lonely = self.write("tree/lonely", data)
        self.write("tree/pad", os.urandom(9999))
        self.sync()
        self.dm("-rd", *WF, self.path("tree"))
        self.assertDmOk()
        self.assertIsNone(self._digest("lonely"), "unique at first -> skipped")

        twin = self.write("tree/twin", data)    # now a same-size duplicate exists
        self.sync()
        self.dm("-rd", *WF, self.path("tree"))
        self.assertDmOk()
        self.sync()
        self.assertIsNotNone(self._digest("lonely"), "now hashed")
        self.assertTrue(files_share(lonely, twin),
                        "previously-skipped file deduped once a twin appeared")

    def test_rerun_is_idempotent(self):
        data = os.urandom(24000)
        self.write("tree/x", data)
        self.write("tree/y", data)
        self.write("tree/uniq", os.urandom(6001))
        self.sync()
        self.dm("-rd", *WF, self.path("tree"))
        self.assertDmOk()
        self.sync()
        self.dm("-rd", *WF, self.path("tree"))
        self.assertDmOk()
        self.assertNoNewSharing()
