"""A read-only subvolume can only ever be a dedupe *source* (#171).

FIDEDUPERANGE rewrites the destination and only the destination, so a member of
a read-only subvolume can legally be the group's target (the source the kernel
reads) and nothing else. Two things follow, and both are tested here:

  * a read-only member must never be used as a destination, and
  * when a group has one, it should be *preferred* as the target -- otherwise
    the group either fails or, on kernels that do not enforce this, silently
    rewrites the extent mapping of a snapshot that is supposed to be immutable.

Whether the kernel refuses is version-dependent: a user on r/btrfs reports
EROFS, while on 7.1.3 the ioctl accepts a read-only destination and rewrites it.
oans therefore refuses on its own rather than relying on the kernel to refuse.

These only apply with --no-skip-readonly-subvols; the default (#156) keeps
read-only subvolumes out of the scan entirely.
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

    def test_writable_copy_is_deduped_onto_the_snapshot(self):
        """The whole point: the saving is taken, in the legal direction.

        The snapshot's copy is deliberately the *more fragmented* one, so the
        pre-#171 least-fragmented rule would have picked the writable file as
        the target and rewritten the snapshot. Read-only-ness has to outrank
        fragmentation for this to come out right.
        """
        data = os.urandom(SIZE)
        snap_file = self._snapshot_of("snap", data, fragmented=True)
        # Contiguous and independently stored (a plain write never reflinks),
        # so fragmentation alone would elect it as target.
        rw = self.write("rw/f.bin", data)
        self.sync()

        snap_before = phys_extents(snap_file)
        self.assertGreater(len(snap_before), len(phys_extents(rw)),
                           "setup: the snapshot copy must be more fragmented")
        self.assertNotShared(snap_file, rw,
                             "setup: the copies must not already share")

        self.dm("-rd", NO_SKIP, os.path.dirname(snap_file), self.path("rw"))
        self.assertDmOk()

        self.assertEqual(snap_before, phys_extents(snap_file),
                         "the read-only snapshot was rewritten")
        self.assertShared(rw, snap_file,
                          "the writable copy was not pointed at the snapshot")

    def test_a_readonly_member_is_never_a_destination(self):
        """With two read-only members, one cannot be the target -- skip it.

        Before #171 the loser was handed to the kernel as a destination, which
        on a permissive kernel rewrites a snapshot's extent mapping.
        """
        data = os.urandom(SIZE)
        a_file = self._snapshot_of("snap_a", data)
        b_file = self._snapshot_of("snap_b", data)
        rw = self.write("rw/f.bin", data)
        self.sync()

        a_before, b_before = phys_extents(a_file), phys_extents(b_file)

        self.dm("-rd", NO_SKIP, os.path.dirname(a_file),
                os.path.dirname(b_file), self.path("rw"))
        self.assertDmOk()

        self.assertEqual(a_before, phys_extents(a_file),
                         "snapshot A was rewritten")
        self.assertEqual(b_before, phys_extents(b_file),
                         "snapshot B was rewritten -- a read-only member was "
                         "used as a dedupe destination")
        # The writable copy still got deduped onto whichever snapshot won.
        self.assertTrue(phys_extents(rw) & (a_before | b_before),
                        "the writable copy was not deduped at all")

    def test_the_skip_is_reported(self):
        """Silently doing less is the failure mode #145/#147/#156 exist to stop."""
        data = os.urandom(SIZE)
        a_file = self._snapshot_of("snap_a", data)
        b_file = self._snapshot_of("snap_b", data)
        self.sync()

        self.dm("-rd", NO_SKIP, os.path.dirname(a_file),
                os.path.dirname(b_file), quiet=False)
        self.assertDmOk()
        self.assertIn("Read-only", self.out,
                      "a skipped read-only destination was not reported")

    def test_default_still_keeps_snapshots_out_of_the_scan(self):
        """#156's behaviour is unchanged; this only affects the opt-in path."""
        data = os.urandom(1 << 20)
        snap_file = self._snapshot_of("snap", data)
        self.write("rw/f.bin", data)
        self.sync()

        snap_dir = os.path.dirname(snap_file)
        self.dm("-rd", snap_dir, self.path("rw"))
        self.assertDmOk()
        self.assertFalse(
            any(p.startswith(snap_dir) for p in self.scanned_files()),
            "the default scanned into a read-only subvolume")


if __name__ == "__main__":
    unittest.main()
