"""Hashing a file must not update its access time.

A scan reads every byte of every file, so without O_NOATIME each one was
stamped as read - which is noise for anyone reading atimes and, on a relatime
mount, one metadata write per file.
"""

import os

from harness import DuperemoveTest

DAY = 86400


class NoAtimeTest(DuperemoveTest):
    def _age_atime(self, path):
        """Put the atime well before the mtime, where relatime updates it."""
        st = os.stat(path)
        os.utime(path, ns=(st.st_mtime_ns - 10 * DAY * 10**9, st.st_mtime_ns))
        return os.stat(path).st_atime_ns

    def test_a_scan_leaves_atime_alone(self):
        probe = self.path("probe")
        self.mkrand("probe", 64 * 1024)
        before = self._age_atime(probe)
        with open(probe, "rb") as f:
            f.read()
        if os.stat(probe).st_atime_ns == before:
            self.skipTest("the scratch filesystem does not update atime "
                          "(mounted noatime?)")

        self.mkrand("tree/a", 256 * 1024)
        path = self.path("tree/a")
        before = self._age_atime(path)
        self.scan(self.path("tree"))
        self.assertDmOk()
        self.assertEqual(1, self.hf_query(
            "select count(*) from files where digest is not null")[0][0],
            "the file was not hashed, so the test proves nothing")
        self.assertEqual(before, os.stat(path).st_atime_ns,
                         "hashing the file updated its atime")
