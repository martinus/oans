"""Hardlinks - regression coverage for the batched-writer bug.

duperemove keeps one filerec per inode (UNIQUE(ino, subvol)). Before the fix, a
second hardlink to an inode within one write batch made the change-detection
read miss the pending row and re-store the inode; the resulting REPLACE deleted
the pending row, cascade-deleted its in-flight hashes, and aborted the whole
batch - so a single hardlink pair could leave the hashfile empty while the
process still exited 0. These tests assert the full, uncorrupted hashfile.
"""

from harness import DuperemoveTest


class HardlinkTest(DuperemoveTest):
    def test_hardlink_pair_does_not_empty_hashfile(self):
        for n in range(400):
            self.mkrand(f"tree/f{n:03d}", 8000)
        # One extra inode reached by two names (the trigger).
        self.mkrand("tree/orig", 15000)
        self.hardlink("tree/orig", "tree/link")

        self.scan(self.path("tree"))
        self.assertDmOk()
        # 400 plain files + 1 shared inode = 401 distinct inodes/rows.
        self.assertEqual(401, self.hf_count("files"), "every inode recorded exactly once")
        # At least one extent per inode. Not exactly one: how many extents a
        # file gets is btrfs's business, and under the load of a parallel suite
        # writeback can split one of these into two (CI saw 402). The bug this
        # test guards emptied the hashfile, so what matters is that every inode
        # has extents at all - the count above is the exact invariant.
        self.assertGreaterEqual(self.hf_count("extents"), 401,
                                "extents present for every inode")

    def test_many_hardlinks_one_row_each(self):
        for n in range(50):
            self.mkrand(f"tree/base{n:02d}", 5000 + n)
            self.hardlink(f"tree/base{n:02d}", f"tree/alias{n:02d}")
        self.scan(self.path("tree"))
        self.assertDmOk()
        self.assertEqual(50, self.hf_count("files"), "50 inodes despite 100 dirents")
        self.assertEqual(
            0,
            self.hf_scalar("select count(*) from (select ino, subvol from files "
                           "group by ino, subvol having count(*) > 1)"),
            "no duplicate inode rows")

    def test_hardlinks_across_batch_boundary(self):
        # The two dirents of a hardlink can land in different write batches
        # (WRITE_BATCH_FILES = 1000). 1500 base inodes; every third also reached
        # by a second name placed far away, so its dirents fall in different
        # batches - proving the guard persists across commits, not just within
        # one open transaction.
        for n in range(1500):
            self.mkrand(f"tree/a{n:05d}", 1200)
        for n in range(0, 1500, 3):
            self.hardlink(f"tree/a{n:05d}", f"tree/z{n:05d}_ln")
        self.scan(self.path("tree"))
        self.assertDmOk()
        self.assertEqual(1500, self.hf_count("files"),
                         "all 1500 inodes recorded across batches")

    def test_hardlinks_rescan_stable(self):
        self.mkrand("tree/orig", 9000)
        self.hardlink("tree/orig", "tree/link")
        self.mkrand("tree/other", 9000)
        self.scan(self.path("tree"))
        self.assertDmOk()
        rows1 = self.hf_count("files")
        self.scan(self.path("tree"))
        self.assertDmOk()
        self.assertEqual(rows1, self.hf_count("files"), "row count stable across rescans")
        self.assertEqual(2, rows1, "orig/link collapse to one inode, plus other")
