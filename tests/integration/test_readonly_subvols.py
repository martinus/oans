"""--skip-readonly-subvols keeps snapshots out of the scan; off by default (#182).

#156 made the skip the default under -d, on the premise that a read-only
subvolume can never be a dedupe *destination* because the kernel refuses it.
That premise is wrong (#171): the kernel deduplicates into a read-only
subvolume quite happily -- only a read-only *mount* is refused -- so snapshots
are ordinary dedupe material, and deduplicating them against each other is how
a snapshot-based backup target is shrunk. The default now includes them.

The option keeps its point for the other shape: where snapshots are near-copies
of the live subvolume they already share its extents, so hashing them reclaims
nothing and the waste scales with snapshot count (20 snapshots of a 1 TiB tree
means reading ~20 TiB). A snapper/Timeshift desktop wants it on.

Detected by property (BTRFS_IOC_GET_SUBVOL_INFO -> BTRFS_ROOT_SUBVOL_RDONLY),
not by directory name, so it catches snapper, Timeshift and Synology alike and
never fires on a writable directory that merely happens to be called
".snapshots".
"""

import json
import os
import unittest

from harness import DuperemoveTest, requires_btrfs

SKIP = "--skip-readonly-subvols"


@requires_btrfs
class ReadonlySubvolTest(DuperemoveTest):
    def setUp(self):
        super().setUp()
        self.live = self.subvol("live")

    def _snapshot(self, name, readonly=True):
        return self.snapshot(self.live, name, readonly=readonly)

    def _live_dups(self):
        """Two duplicate files inside the live subvolume."""
        data = os.urandom(200000)
        for name in ("a.bin", "b.bin"):
            with open(os.path.join(self.live, name), "wb") as f:
                f.write(data)
        self.sync()

    def _scanned(self):
        return {os.path.basename(os.path.dirname(row[0]))
                for row in self.hf_query("select filename from files")}

    def test_dedupe_covers_a_readonly_snapshot_by_default(self):
        """The point of the flip: a backup target made of snapshots is served."""
        self._live_dups()
        self._snapshot("snap1")
        self.dedupe(self.work)
        self.assertDmOk()
        self.assertIn("snap1", self._scanned(),
                      "the read-only snapshot was skipped by default")
        self.assertIn("live", self._scanned())

    def test_report_mode_covers_snapshots(self):
        """"What duplicates exist in my snapshots?" is fair to ask."""
        self._live_dups()
        self._snapshot("snap1")
        self.scan(self.work)
        self.assertDmOk()
        self.assertIn("snap1", self._scanned(),
                      "report mode should not skip snapshots")

    def test_opt_in_keeps_them_out(self):
        """No read, no hash, no hashfile rows -- what the option is for."""
        self._live_dups()
        self._snapshot("snap1")
        self.dm("-rd", SKIP, self.work)
        self.assertDmOk()
        self.assertNotIn("snap1", self._scanned(),
                         "the read-only snapshot was hashed anyway")
        self.assertIn("live", self._scanned())

    def test_skip_is_reported_not_silent(self):
        """A scan that quietly shrinks is the #147 failure mode."""
        self._live_dups()
        self._snapshot("snap1")
        self.dm("-rd", SKIP, self.work, quiet=False)
        self.assertIn("read-only subvolume", self.out, "the skip was silent")

    def test_skip_is_counted_per_subvolume_not_per_file(self):
        self._live_dups()
        self._snapshot("snap1")
        self._snapshot("snap2")
        self.dm("-rd", SKIP, self.work, quiet=False)
        self.assertIn("2 read-only subvolumes skipped", self.out)

    def test_default_says_nothing_about_snapshots(self):
        """Nothing is skipped, so there is no shrunk-scan line to report."""
        self._live_dups()
        self._snapshot("snap1")
        self.dedupe(self.work, "--no-skip-readonly-subvols")
        self.assertDmOk()
        self.assertNotIn("read-only subvolume", self.out)

    def test_skip_survives_a_replay(self):
        """The choice is scan-shaping, so the hashfile must remember it.

        Otherwise a scheduled job's scope would silently change the first time
        it replayed, which is exactly what the stored-config design prevents.
        """
        self._live_dups()
        self._snapshot("snap1")
        self.dm("-rd", SKIP, self.work)
        rows_before = len(self.hf_query("select filename from files"))

        # Bare replay: no paths, no options.
        self.dm(hashfile=True)
        self.assertDmOk()
        self.assertEqual(rows_before,
                         len(self.hf_query("select filename from files")),
                         "the replay scanned the snapshot the skip excluded")

    def test_writable_snapshot_is_scanned_even_when_skipping(self):
        """Read-only-ness is the criterion, not "is a snapshot"."""
        self._live_dups()
        self._snapshot("snap1", readonly=False)
        self.dm("-rd", SKIP, self.work)
        self.assertDmOk()
        self.assertIn("snap1", self._scanned(),
                      "a writable snapshot must still be deduplicated")

    def test_skip_reaches_json(self):
        self._live_dups()
        self._snapshot("snap1")
        self.dm("-rd", SKIP, self.work)
        metrics = json.loads(self.dm("--json"))
        self.assertEqual(1, metrics["readonly_subvols_skipped_last_run"])
        # A saving, not a fault: it must not raise the error total.
        self.assertEqual(0, metrics["scan_skipped_errors_total"])


if __name__ == "__main__":
    unittest.main()
