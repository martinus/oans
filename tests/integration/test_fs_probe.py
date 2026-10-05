"""#224: what oans can deduplicate is probed, not hardcoded.

btrfs and XFS stay on an allowlist fast path, so their behaviour is exactly
what it always was. Everything else is asked directly for FIDEDUPERANGE with a
real block-sized request, reading the per-destination status as well as errno
(a zero-length request cannot tell a pass-through filesystem like overlayfs
from one that can really dedupe).

That leaves the accept branch untestable by ordinary means: the only
filesystems known to answer "yes" are the two that never reach the probe.
DUPEREMOVE_FORCE_FS_PROBE drops the fast path so the probe runs against the
scratch filesystem too -- which is how these tests reach the "yes" branch on a
filesystem that really does implement the ioctl. Without it the code path would
ship having only ever been seen to say "no".

The refusal side lives in test_unsupported_fs.py, which needs a non-reflink
filesystem to point at.
"""

import os

from harness import DuperemoveTest, requires_reflink

MiB = 1 << 20
FORCED = {"DUPEREMOVE_FORCE_FS_PROBE": "1"}


@requires_reflink
class FsProbeTest(DuperemoveTest):
    def test_a_forced_probe_still_dedupes(self):
        """The probe says yes on a real reflink fs, and the run is unaffected."""
        a, b = self.mkdup("tree/a.bin", "tree/b.bin", 1 * MiB)
        self.sync()

        self.dm("-rd", self.path("tree"), env=FORCED)
        self.assertDmOk("a forced probe must not disturb a supported filesystem")
        self.sync()
        self.assertShared(a, b, "the files were not deduped under a forced probe")

    def test_a_forced_probe_changes_nothing(self):
        """The same hashfile as a run that took the fast path.

        The probe is meant to be invisible on a filesystem that supports the
        ioctl. fingerprints() is the comparison for that (files, extents and
        blocks) - the probe issues a real dedupe request, so a weaker check on
        file digests alone would miss it perturbing the extent rows, which is
        exactly where it could show.
        """
        self.mkdup("tree/a.bin", "tree/b.bin", 1 * MiB)
        self.mkrand("tree/c.bin", 512 * 1024)
        self.sync()

        self.scan(self.path("tree"))
        self.assertDmOk()
        baseline = self.fingerprints()

        self.drop_hashfile()
        self.dm("-r", self.path("tree"), env=FORCED)
        self.assertDmOk()

        self.assertEqual(baseline, self.fingerprints(),
                         "a forced probe changed what the scan stored")

    def test_the_probe_leaves_the_file_alone(self):
        """The probe must not change the file it is asked about.

        It issues a real dedupe request, so on a filesystem that supports the
        ioctl its two ranges may genuinely be shared -- that is the trade for
        seeing through a pass-through filesystem. What must never change is
        what anyone can observe: contents, size, mtime. A regression here would
        corrupt the first file of every scan on an unrecognised filesystem.
        """
        p = self.mkrand("tree/only.bin", 1 * MiB)
        before = self.tree_digest(self.path("tree"))
        st_before = os.stat(p)
        self.sync()

        self.dm("-r", self.path("tree"), env=FORCED)
        self.assertDmOk()

        self.assertEqual(before, self.tree_digest(self.path("tree")),
                         "the probe altered the file's contents")
        st_after = os.stat(p)
        self.assertEqual(st_before.st_size, st_after.st_size,
                         "the probe altered the file's size")
        self.assertEqual(st_before.st_mtime, st_after.st_mtime,
                         "the probe altered the file's mtime")


@requires_reflink
class FsProbeBudgetTest(DuperemoveTest):
    """A file that cannot host the probe is not asked, and does not spend
    FS_PROBE_MAX_TRIES (16). Counting them refused a supported filesystem
    whenever the walk handed over enough small or read-only files first.

    One walker, so the files at the top come out before the one below them.
    """
    COUNT = 40          # well past FS_PROBE_MAX_TRIES

    def run_forced(self):
        self.dm("-r", "--io-threads=1", self.path("tree"), env=FORCED)

    def test_small_files_first_do_not_spend_the_budget(self):
        for i in range(self.COUNT):
            self.mkrand(f"tree/f{i:02}", 100)   # under two blocks
        self.mkrand("tree/z/big.bin", 1 * MiB)
        self.run_forced()
        self.assertDmOk("the large file settles it; the small ones say "
                        "nothing about the filesystem")
        self.assertEqual(self.COUNT + 1, len(self.scanned_files()))

    def test_read_only_files_first_do_not_spend_the_budget(self):
        """Owned but 0444: asked through a read-only fd, as the dedupe phase
        opens them. (As root they open read-write anyway.)"""
        for i in range(self.COUNT):
            os.chmod(self.mkrand(f"tree/f{i:02}", 1 * MiB), 0o444)
        self.run_forced()
        self.assertDmOk()
        self.assertEqual(self.COUNT, len(self.scanned_files()))

    def test_a_tree_no_file_can_test_is_still_refused(self):
        """Not asking is not an answer: the walk ends unsettled, and says
        why."""
        for i in range(self.COUNT):
            self.mkrand(f"tree/f{i:02}", 100)
        self.run_forced()
        self.assertNotEqual(0, self.rc, self.out)
        self.assertIn(f"{self.COUNT} file(s) were too small", self.out)
