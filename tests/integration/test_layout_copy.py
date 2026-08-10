"""Snapshot-aware scan: identical physical layout means identical bytes (#206).

Hashing N snapshots of a subvolume used to read N times the data. A snapshot
shares the live subvolume's extents, so when two files' fiemaps describe exactly
the same records their content is the same and the second one's digest, extent
hashes and block hashes can be copied instead of computed.

The only acceptable definition of correct here is byte-identical output: a wrong
digest is invisible downstream (it just looks like a file with no duplicate), so
every test below compares the hashfile against a control run with
DUPEREMOVE_NO_LAYOUT_COPY=1, which hashes everything the old way.

Physical-layout assertions, so serial - see DuperemoveTest.serial.
"""

import os

from harness import DuperemoveTest, requires_btrfs, requires_reflink

MiB = 1 << 20

# The optimisation is skipped below one read buffer, where reading the file is a
# single I/O; these have to clear that.
SIZE = 2 * MiB


@requires_btrfs
class LayoutCopyTest(DuperemoveTest):
    serial = True

    def setUp(self):
        super().setUp()
        self.live = self.subvol("live")

    def _fill(self, n=4, size=SIZE):
        for i in range(n):
            with open(os.path.join(self.live, f"f{i}.bin"), "wb") as f:
                f.write(os.urandom(size))
        self.sync()

    def _scan_both_ways(self, *extra):
        """Scan the scratch tree with the copy on, then off; return both prints.

        Same tree, two fresh hashfiles - the control is what the scan would have
        produced before this feature existed.

        --io-threads=1 makes the hit deterministic. A file only has a donor if
        some other file *finished* hashing first, so on a tree this small the
        original and its snapshot are otherwise hashed side by side and neither
        can donate to the other. That race is fine in production - a miss just
        costs a hash - but it would make these tests flaky.
        """
        self.dm("-r", "--io-threads=1", self.work, *extra, quiet=False)
        self.assertDmOk("layout-copy scan")
        with_copy = self.fingerprints()
        copied = self.layout_copies()

        self.drop_hashfile()
        self.dm("-r", "--io-threads=1", self.work, *extra, quiet=False,
                env={"DUPEREMOVE_NO_LAYOUT_COPY": "1"})
        self.assertDmOk("control scan")
        self.assertEqual(0, self.layout_copies(),
                         "the kill switch did not switch anything off")
        return with_copy, self.fingerprints(), copied

    def layout_copies(self):
        """Files whose hashes were copied, from the last run's summary."""
        for line in self.out.splitlines():
            if "matched an already-hashed layout" in line:
                return int(line.split()[2])
        return 0

    def test_a_snapshot_is_not_read_again(self):
        self._fill()
        self.snapshot(self.live, "snap")
        self.sync()

        copy, control, copied = self._scan_both_ways()
        self.assertEqual(control, copy,
                         "the copied hashes differ from hashing the file")
        self.assertGreater(copied, 0, "nothing matched an identical layout")

    def test_block_hashes_are_copied_too(self):
        # --dedupe-options=partial is the only mode that stores block hashes,
        # and they are the bulk of what a copy avoids recomputing.
        self._fill(n=2)
        self.snapshot(self.live, "snap")
        self.sync()

        copy, control, copied = self._scan_both_ways(
            "-b", "4096", "--dedupe-options=partial")
        self.assertEqual(control, copy)
        self.assertGreater(copied, 0)
        self.assertGreater(self.hf_count("blocks"), 0)

    def test_a_modified_file_is_hashed_normally(self):
        """The negative case: a rewrite moves the file's extents, so it misses.

        A writable snapshot whose file is rewritten no longer shares the
        original's storage; copying its digest would be the catastrophic
        failure this feature has to avoid.
        """
        self._fill(n=2)
        snap = self.snapshot(self.live, "snap", readonly=False)
        changed = os.path.join(snap, "f0.bin")
        with open(changed, "r+b") as f:
            f.write(os.urandom(SIZE))       # CoW: new extents
        self.sync()

        copy, control, _copied = self._scan_both_ways()
        self.assertEqual(control, copy)

        # And the rewritten copy really did end up with its own digest.
        rows = dict(self.hf_query("select filename, quote(digest) from files"))
        self.assertNotEqual(rows[os.path.join(self.live, "f0.bin")],
                            rows[changed],
                            "a rewritten file kept the original's digest")

    def test_small_files_are_left_alone(self):
        """Below one read buffer there is nothing to save, so no bookkeeping."""
        self._fill(n=4, size=64 << 10)
        self.snapshot(self.live, "snap")
        self.sync()

        copy, control, copied = self._scan_both_ways()
        self.assertEqual(control, copy)
        self.assertEqual(0, copied)

    @requires_reflink
    def test_dedupe_still_works_on_copied_hashes(self):
        """Copied rows have to be as good as hashed ones downstream."""
        self._fill(n=2)
        self.snapshot(self.live, "snap", readonly=False)
        self.sync()

        self.dm("-rd", self.work, quiet=False)
        self.assertDmOk("dedupe over a snapshot tree")

        # Converges: a second run over the unchanged tree frees nothing. The
        # invariant #186 exists to protect, and the one a bogus copied digest
        # would break first.
        self.dm("-rd", self.work, quiet=False)
        self.assertDmOk("second dedupe")
        self.assertReclaimedNothing("the tree is already deduped")

    @requires_reflink
    def test_it_works_without_a_hashfile_too(self):
        """No --hashfile is not "no database" - it is an in-memory one.

        The donor's rows are read back through the same handle the scan wrote
        them with, so the copy works there exactly as it does on disk. What
        matters is that the outcome is identical either way.
        """
        self._fill(n=2)
        self.snapshot(self.live, "snap", readonly=False)
        # An independently written pair, so the run has something to reclaim.
        self.mkdup("live/dupA.bin", "live/dupB.bin", SIZE)
        self.sync()

        self.dm("-r", "--io-threads=1", self.work, hashfile=False, quiet=False)
        self.assertDmOk()
        self.assertGreater(self.layout_copies(), 0,
                           "no --hashfile should not switch the copy off")

    def test_the_kill_switch_is_the_only_thing_that_disables_it(self):
        self._fill(n=2)
        self.snapshot(self.live, "snap")
        self.sync()

        self.dm("-r", "--io-threads=1", self.work, quiet=False,
                env={"DUPEREMOVE_NO_LAYOUT_COPY": "1"})
        self.assertDmOk()
        self.assertEqual(0, self.layout_copies())
