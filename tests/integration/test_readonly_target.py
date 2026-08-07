"""A read-only subvolume is *preferred* as the dedupe source, never refused (#171).

FIDEDUPERANGE rewrites the destination and only the destination, so which member
of a group becomes the target decides which file gets rewritten. Where a group
has a member in a read-only subvolume, electing it as the target leaves the
snapshot alone and rewrites the writable copies instead.

It is only a preference. The kernel deduplicates *into* a read-only subvolume
quite happily: the content is byte-verified and never changes, only which
extents back it, and people have relied on that since at least 2019 to shrink
snapshot-based backups. Refusing it broke that workflow (see
test_two_readonly_snapshots_still_dedupe, which is the regression guard).

The EROFS people do sometimes hit comes from a read-only *mount*, not from the
subvolume flag: vfs_dedupe_file_range_one() calls mnt_want_write_file(), which
tests the mount. A read-only subvolume under an ordinary read-write mount does
not trip it, which is why the same operation works for most people and fails for
someone whose snapshots are mounted -o ro.

Snapshots are in the scan by default (#182); --skip-readonly-subvols is the
opt-out. The tests below pass --no-skip-readonly-subvols explicitly anyway, so
they keep testing target selection rather than whatever the default is.
"""

import os
import unittest

from harness import DuperemoveTest, phys_extents, requires_btrfs

NO_SKIP = "--no-skip-readonly-subvols"
SIZE = 1 << 21


@requires_btrfs
class ReadonlyTargetTest(DuperemoveTest):
    # Asserts on the physical extent layout, so it must not race btrfs
    # writeback against the rest of the suite.
    serial = True

    def _snapshot_of(self, name, data, fragmented=False):
        """A read-only snapshot holding f.bin with `data`."""
        live = self.subvol(f"live_{name}")
        f = os.path.join(live, "f.bin")
        if fragmented:
            self.fragment(f, data)
        else:
            with open(f, "wb") as fh:
                fh.write(data)
        self.sync()
        snap = self.snapshot(live, name)
        return os.path.join(snap, "f.bin")

    def test_two_readonly_snapshots_still_dedupe(self):
        """Regression: refusing read-only destinations broke this outright.

        Deduplicating snapshots against each other is the whole point of
        running offline dedup on a backup target, and the kernel allows it.
        oans must not second-guess that -- especially not when the user asked
        for the snapshots to be included in the first place.
        """
        data = os.urandom(SIZE)
        a_file = self._snapshot_of("snap_a", data)
        b_file = self._snapshot_of("snap_b", data)
        self.sync()

        self.assertNotShared(a_file, b_file, "setup: must start unshared")

        self.dm("-rd", NO_SKIP, os.path.dirname(a_file),
                os.path.dirname(b_file))
        self.assertDmOk()

        self.assertShared(a_file, b_file,
                          "two read-only snapshots were not deduplicated")

    def test_a_readonly_member_is_preferred_as_the_target(self):
        """The snapshot is left alone; the writable copy is the one rewritten.

        The snapshot's copy is deliberately the *more fragmented* one, so the
        least-fragmented rule alone would have elected the writable file and
        rewritten the snapshot instead.
        """
        data = os.urandom(SIZE)
        snap_file = self._snapshot_of("snap", data, fragmented=True)
        rw = self.write("rw/f.bin", data)
        self.sync()

        snap_before = phys_extents(snap_file)
        self.assertGreater(len(snap_before), len(phys_extents(rw)),
                           "setup: the snapshot copy must be more fragmented")
        self.assertNotShared(snap_file, rw, "setup: must start unshared")

        self.dm("-rd", NO_SKIP, os.path.dirname(snap_file), self.path("rw"))
        self.assertDmOk()

        self.assertEqual(snap_before, phys_extents(snap_file),
                         "the snapshot was rewritten instead of the copy")
        self.assertShared(rw, snap_file,
                          "the writable copy was not pointed at the snapshot")

    def test_the_default_reaches_into_a_readonly_subvolume(self):
        """No flag needed: a snapshot-based backup target dedupes out of the box.

        The counterpart to test_two_readonly_snapshots_still_dedupe -- that one
        proves the dedupe is allowed, this one proves the scan gets there at all,
        which is what #156's default used to prevent (#182).
        """
        data = os.urandom(1 << 20)
        snap_file = self._snapshot_of("snap", data)
        self.write("rw/f.bin", data)
        self.sync()

        snap_dir = os.path.dirname(snap_file)
        self.dm("-rd", snap_dir, self.path("rw"))
        self.assertDmOk()
        self.assertTrue(
            any(p.startswith(snap_dir) for p in self.scanned_files()),
            "the default did not scan into a read-only subvolume")
        self.assertShared(self.path("rw/f.bin"), snap_file,
                          "the writable copy was not deduped against the snapshot")


if __name__ == "__main__":
    unittest.main()
