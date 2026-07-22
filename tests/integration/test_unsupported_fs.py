"""Pointing oans at a non-btrfs/XFS filesystem must fail loudly, not exit 0.

Regression guard: an unsupported seed root used to be accepted, walked, and
then fail every FIEMAP with per-file errors while still exiting 0 ("Nothing to
deduplicate"). It is now rejected up front, and a run whose roots are all
unsupported exits non-zero with a clear message.
"""

import os
import shutil
import tempfile

from harness import DuperemoveTest, scratch_fstype


class UnsupportedFsTest(DuperemoveTest):
    def test_unsupported_fs_fails_loudly(self):
        # Need a directory that is NOT on btrfs/XFS. The system temp dir is
        # ext4 (CI) or tmpfs (most dev boxes); skip if it happens to be a
        # reflink fs so we don't misjudge a supported setup.
        outside = tempfile.mkdtemp(prefix="oans-unsupported.")
        self.addCleanup(shutil.rmtree, outside, ignore_errors=True)
        fstype = scratch_fstype(outside)
        if fstype in ("btrfs", "xfs"):
            self.skipTest(f"system temp dir is {fstype}, a supported fs")

        with open(os.path.join(outside, "a.bin"), "wb") as f:
            f.write(os.urandom(1 << 20))

        self.dedupe(outside)   # oans -rd on the unsupported tree

        self.assertNotEqual(
            0, self.rc,
            "oans must exit non-zero when no path is on a supported filesystem")
        self.assertIn(
            "not btrfs or XFS", self.out,
            "a clear unsupported-filesystem message is printed")
        # The root is refused, not walked, so no per-file FIEMAP error spam.
        self.assertNotIn(
            "fiemap", self.out.lower(),
            "unsupported roots are skipped before any FIEMAP call")
