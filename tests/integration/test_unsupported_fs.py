"""Pointing oans at a non-btrfs/XFS filesystem must fail loudly, not exit 0.

Regression guard: an unsupported seed root used to be accepted, walked, and
then fail every FIEMAP with per-file errors while still exiting 0 ("Nothing to
deduplicate"). It is now refused with a clear message and a non-zero exit,
before anything is hashed.

Since #224 the refusal comes from probing the filesystem for FIDEDUPERANGE
rather than from matching its name against btrfs and XFS, so the message names
the missing ioctl. What must not change is the outcome: loud, non-zero, and no
per-file FIEMAP spam.
"""

import os
import shutil
import tempfile

from harness import DuperemoveTest, scratch_fstype


class UnsupportedFsTest(DuperemoveTest):
    def unsupported_dir(self):
        """A scratch directory that is NOT on btrfs/XFS, or skip the test.

        The system temp dir is ext4 (CI) or tmpfs (most dev boxes); skip if it
        happens to be a reflink fs so we don't misjudge a supported setup.
        """
        outside = tempfile.mkdtemp(prefix="oans-unsupported.")
        self.addCleanup(shutil.rmtree, outside, ignore_errors=True)
        fstype = scratch_fstype(outside)
        if fstype in ("btrfs", "xfs"):
            self.skipTest(f"system temp dir is {fstype}, a supported fs")
        return outside

    def test_unsupported_fs_fails_loudly(self):
        outside = self.unsupported_dir()

        with open(os.path.join(outside, "a.bin"), "wb") as f:
            f.write(os.urandom(1 << 20))

        self.dedupe(outside)   # oans -rd on the unsupported tree

        self.assertNotEqual(
            0, self.rc,
            "oans must exit non-zero when no path is on a supported filesystem")
        self.assertIn(
            "FIDEDUPERANGE", self.out,
            "the message names the ioctl the filesystem is missing")
        # The root is refused, not walked, so no per-file FIEMAP error spam.
        self.assertNotIn(
            "fiemap", self.out.lower(),
            "unsupported roots are skipped before any FIEMAP call")

    def test_an_unsupported_fs_with_nothing_to_scan_still_fails(self):
        """No file to probe is not the same as a clean run.

        Since #224 the verdict comes from asking a file, so a tree with no
        scannable file leaves the question unanswered. Exiting 0 there would be
        exactly the silent no-op the upfront rejection was introduced to kill --
        the user pointed oans at a filesystem it may not be able to use at all
        and would have been told nothing.
        """
        outside = self.unsupported_dir()

        # Empty files: below --min-filesize, so none of them reaches the
        # scanner and nothing can answer the probe.
        for i in range(4):
            open(os.path.join(outside, f"empty{i}"), "wb").close()

        self.dedupe(outside)

        self.assertNotEqual(
            0, self.rc,
            "a filesystem that was never proved usable must not exit 0")
        self.assertIn("FIDEDUPERANGE", self.out,
                      "the message says what could not be established")
