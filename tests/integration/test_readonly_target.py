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
import subprocess
import unittest

from harness import DuperemoveTest, phys_extents, requires_btrfs

NO_SKIP = "--no-skip-readonly-subvols"


def _btrfs(*args):
    return subprocess.run(["btrfs", *args], stdout=subprocess.DEVNULL,
                          stderr=subprocess.DEVNULL).returncode == 0


@requires_btrfs
class ReadonlyTargetTest(DuperemoveTest):
    # Asserts on the physical extent layout, so it must not race btrfs
    # writeback against the rest of the suite.
    serial = True

    def setUp(self):
        super().setUp()
        self.subvols = []

    def tearDown(self):
        for path in reversed(self.subvols):
            _btrfs("subvolume", "delete", path)
        super().tearDown()

    def _subvol(self, name):
        p = os.path.join(self.work, name)
        if not _btrfs("subvolume", "create", p):
            self.skipTest("cannot create a btrfs subvolume here")
        self.subvols.append(p)
        return p

    def _snapshot(self, src, name):
        dst = os.path.join(self.work, name)
        self.assertTrue(_btrfs("subvolume", "snapshot", "-r", src, dst),
                        f"could not snapshot into {name}")
        self.subvols.append(dst)
        return dst

    def _independent_copy(self, src, rel):
        """A byte-identical copy that shares no extents with src."""
        dst = self.path(rel)
        subprocess.run(["cp", "--reflink=never", src, dst], check=True)
        return dst

    def _fragment(self, path, content):
        """Write content, then rewrite alternate 4K blocks so COW splits it."""
        with open(path, "wb") as f:
            f.write(content)
            f.flush()
            os.fsync(f.fileno())
        fd = os.open(path, os.O_WRONLY)
        try:
            for off in range(0, len(content), 8192):
                os.pwrite(fd, content[off:off + 4096], off)
                os.fsync(fd)
        finally:
            os.close(fd)

    def test_writable_copy_is_deduped_onto_the_snapshot(self):
        """The whole point: the saving is taken, in the legal direction.

        The snapshot's copy is deliberately the *more fragmented* one, so the
        pre-#171 least-fragmented rule would have picked the writable file as
        the target and rewritten the snapshot. Read-only-ness has to outrank
        fragmentation for this to come out right.
        """
        live = self._subvol("live")
        data = os.urandom(1 << 21)
        self._fragment(os.path.join(live, "f.bin"), data)
        self.sync()
        snap = self._snapshot(live, "snap")
        # A contiguous, independently-stored copy: fewer extents than the
        # snapshot's, so fragmentation alone would elect it as target.
        rw = self.write("rw/f.bin", data)
        self.sync()

        snap_file = os.path.join(snap, "f.bin")
        snap_before = phys_extents(snap_file)
        self.assertGreater(len(snap_before), len(phys_extents(rw)),
                           "setup: the snapshot copy must be more fragmented")
        self.assertNotEqual(snap_before, phys_extents(rw),
                            "setup is wrong: the copies already share extents")

        self.dm("-rd", NO_SKIP, snap, self.path("rw"))
        self.assertDmOk()

        self.assertEqual(snap_before, phys_extents(snap_file),
                         "the read-only snapshot was rewritten")
        self.assertTrue(phys_extents(rw) & snap_before,
                        "the writable copy was not pointed at the snapshot")

    def test_a_readonly_member_is_never_a_destination(self):
        """With two read-only members, one cannot be the target -- skip it.

        Before #171 the loser was handed to the kernel as a destination, which
        on a permissive kernel rewrites a snapshot's extent mapping.
        """
        live_a = self._subvol("live_a")
        live_b = self._subvol("live_b")
        data = os.urandom(1 << 21)
        for sv in (live_a, live_b):
            with open(os.path.join(sv, "f.bin"), "wb") as f:
                f.write(data)
        self.sync()
        snap_a = self._snapshot(live_a, "snap_a")
        snap_b = self._snapshot(live_b, "snap_b")
        rw = self._independent_copy(os.path.join(snap_a, "f.bin"), "rw/f.bin")
        self.sync()

        a_file, b_file = os.path.join(snap_a, "f.bin"), os.path.join(snap_b, "f.bin")
        a_before, b_before = phys_extents(a_file), phys_extents(b_file)

        self.dm("-rd", NO_SKIP, snap_a, snap_b, self.path("rw"))
        self.assertDmOk()

        self.assertEqual(a_before, phys_extents(a_file),
                         "snapshot A was rewritten")
        self.assertEqual(b_before, phys_extents(b_file),
                         "snapshot B was rewritten -- a read-only member was "
                         "used as a dedupe destination")
        # The writable copy still got deduped onto whichever snapshot won.
        self.assertTrue(phys_extents(rw) in (a_before, b_before),
                        "the writable copy was not deduped at all")

    def test_the_skip_is_reported(self):
        """Silently doing less is the failure mode #145/#147/#156 exist to stop."""
        live_a = self._subvol("live_a")
        live_b = self._subvol("live_b")
        data = os.urandom(1 << 21)
        for sv in (live_a, live_b):
            with open(os.path.join(sv, "f.bin"), "wb") as f:
                f.write(data)
        self.sync()
        snap_a = self._snapshot(live_a, "snap_a")
        snap_b = self._snapshot(live_b, "snap_b")
        self.sync()

        self.dm("-rd", NO_SKIP, snap_a, snap_b, quiet=False)
        self.assertDmOk()
        self.assertIn("Read-only", self.out,
                      "a skipped read-only destination was not reported")

    def test_default_still_keeps_snapshots_out_of_the_scan(self):
        """#156's behaviour is unchanged; this only affects the opt-in path."""
        live = self._subvol("live")
        data = os.urandom(1 << 20)
        with open(os.path.join(live, "f.bin"), "wb") as f:
            f.write(data)
        self.sync()
        snap = self._snapshot(live, "snap")
        self._independent_copy(os.path.join(snap, "f.bin"), "rw/f.bin")
        self.sync()

        self.dm("-rd", snap, self.path("rw"))
        self.assertDmOk()
        scanned = {r[0] for r in self.hf_query("select filename from files")}
        self.assertFalse(any(s.startswith(snap) for s in scanned),
                         "the default scanned into a read-only subvolume")


if __name__ == "__main__":
    unittest.main()
