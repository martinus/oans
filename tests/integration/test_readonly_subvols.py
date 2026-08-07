"""Read-only btrfs subvolumes are dead weight under -d (#156).

Pointed at the normal shape of a NAS or a snapper/Timeshift desktop, oans used
to walk into every read-only snapshot and read and hash every file in it. The
behaviour was *safe* -- the already-shared check catches them before any ioctl
-- but by then all the read and hash I/O had been spent, and the waste scales
with snapshot count: 20 snapshots of a 1 TiB tree means reading ~20 TiB to
reclaim nothing from 19 of them.

Detected by property (BTRFS_IOC_GET_SUBVOL_INFO -> BTRFS_ROOT_SUBVOL_RDONLY),
not by directory name, so it catches snapper, Timeshift and Synology alike and
never fires on a writable directory that merely happens to be called
".snapshots".
"""

import json
import os
import unittest

from harness import DuperemoveTest, requires_btrfs


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

    def test_dedupe_does_not_hash_a_readonly_snapshot(self):
        """The whole point: no read, no hash, no hashfile rows."""
        self._live_dups()
        self._snapshot("snap1")
        self.dedupe(self.work)
        self.assertDmOk()
        self.assertNotIn("snap1", self._scanned(),
                         "the read-only snapshot was hashed anyway")
        self.assertIn("live", self._scanned())

    def test_report_mode_still_covers_snapshots(self):
        """Without -d, "what duplicates exist in my snapshots?" is fair to ask."""
        self._live_dups()
        self._snapshot("snap1")
        self.scan(self.work)
        self.assertDmOk()
        self.assertIn("snap1", self._scanned(),
                      "report mode should not skip snapshots")

    def test_skip_is_reported_not_silent(self):
        """A default that quietly shrinks the scan is the #147 failure mode."""
        self._live_dups()
        self._snapshot("snap1")
        self.dedupe(self.work)
        self.assertIn("read-only subvolume", self.out,
                      "the skip was silent")

    def test_skip_is_counted_per_subvolume_not_per_file(self):
        self._live_dups()
        self._snapshot("snap1")
        self._snapshot("snap2")
        self.dedupe(self.work)
        self.assertIn("2 read-only subvolumes skipped", self.out)

    def test_opt_out_scans_them_again(self):
        self._live_dups()
        self._snapshot("snap1")
        self.dm("-rd", "--no-skip-readonly-subvols", self.work)
        self.assertDmOk()
        self.assertIn("snap1", self._scanned(),
                      "--no-skip-readonly-subvols was ignored")

    def test_opt_out_survives_a_replay(self):
        """The choice is scan-shaping, so the hashfile must remember it.

        Otherwise a scheduled job's scope would silently change the first time
        it replayed, which is exactly what the stored-config design prevents.
        """
        self._live_dups()
        self._snapshot("snap1")
        self.dm("-rd", "--no-skip-readonly-subvols", self.work)
        rows_before = len(self.hf_query("select filename from files"))

        # Bare replay: no paths, no options.
        self.dm(hashfile=True)
        self.assertDmOk()
        self.assertEqual(rows_before,
                         len(self.hf_query("select filename from files")),
                         "the replay dropped the snapshot the opt-out included")

    def test_writable_snapshot_is_still_scanned(self):
        """Read-only-ness is the criterion, not "is a snapshot"."""
        self._live_dups()
        self._snapshot("snap1", readonly=False)
        self.dedupe(self.work)
        self.assertDmOk()
        self.assertIn("snap1", self._scanned(),
                      "a writable snapshot must still be deduplicated")

    def test_skip_reaches_json(self):
        self._live_dups()
        self._snapshot("snap1")
        self.dedupe(self.work)
        metrics = json.loads(self.dm("--json"))
        self.assertEqual(1, metrics["readonly_subvols_skipped_last_run"])
        # A saving, not a fault: it must not raise the error total.
        self.assertEqual(0, metrics["scan_skipped_errors_total"])


if __name__ == "__main__":
    unittest.main()
