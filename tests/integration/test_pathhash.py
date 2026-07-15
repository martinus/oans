"""Path-identity semantics that the files-table uniqueness constraint enforces.

These pin the behavior that UNIQUE(path_hash) must preserve versus the old
UNIQUE(filename): one row per path, path reuse evicts the stale row, and
removal-by-path still works. They are written against behavior (not schema), so
they pass on both the path-hash build and stock duperemove.
"""

import os
from harness import DuperemoveTest


class PathIdentityTest(DuperemoveTest):
    def test_path_reuse_by_new_inode_evicts_stale_row(self):
        # A file at path P is scanned, then deleted and recreated at the same
        # path with a different inode. The upsert conflicts on the path and must
        # evict the stale (old-inode) row, leaving exactly one row for P.
        p = self.mkrand("tree/f", 10000)
        self.scan(self.path("tree"))
        ino1 = self.hf_scalar("select ino from files where filename like '%/f'")

        os.remove(p)
        self.mkrand("tree/f", 12000)                 # new content -> new inode
        ino2 = os.stat(self.path("tree/f")).st_ino
        if ino1 == ino2:
            self.skipTest("inode was reused; cannot exercise path-reuse path")

        self.scan(self.path("tree"))
        self.assertDmOk()
        self.assertEqual(1, self.hf_count("files"), "one row per path after reuse")
        self.assertEqual(ino2, self.hf_scalar("select ino from files where filename like '%/f'"),
                         "stale-inode row evicted, new inode kept")

    def test_remove_by_path(self):
        # -R removes the named path's row and leaves the others intact.
        self.mkrand("tree/keep1", 8000)
        target = self.mkrand("tree/gone", 8000)
        self.mkrand("tree/keep2", 8000)
        self.scan(self.path("tree"))
        self.assertEqual(3, self.hf_count("files"))

        self.dm("-R", target)                        # remove just this path
        self.assertDmOk()
        self.assertEqual(2, self.hf_count("files"), "one row removed")
        self.assertEqual(
            0, self.hf_scalar("select count(*) from files where filename like '%/gone'"),
            "the removed path is gone")
        self.assertEqual(
            2, self.hf_scalar("select count(*) from files where filename like '%/keep%'"),
            "the other rows are untouched")

    def test_remove_nonexistent_path_is_noop(self):
        # Removing a path not in the hashfile changes nothing (and must not
        # remove a different path that happens to share a hash bucket).
        self.mkrand("tree/a", 8000)
        self.mkrand("tree/b", 8000)
        self.scan(self.path("tree"))
        self.dm("-R", self.path("tree/not-there"))
        self.assertDmOk()
        self.assertEqual(2, self.hf_count("files"), "no rows removed for a missing path")
