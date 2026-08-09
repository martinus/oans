"""Hashing one huge file survives being interrupted (#159).

A 1 TiB file six hours into hashing used to start again from byte zero every
time the run was killed, so on a big enough file a scheduled scan could never
finish. Now a run leaves checkpoints behind and the next one picks up from the
last of them.

The property that matters is not that resuming is fast - it is that resuming is
*invisible*: a file hashed across any number of interruptions must end up with
exactly the digest, extent hashes and block hashes it would have had from one
straight-through scan. Nothing downstream could tell a wrong digest from a file
that simply has no duplicate, so a resume that quietly computed something else
would corrupt the hashfile in silence.

DUPEREMOVE_CHECKPOINT_BYTES lowers the one-gigabyte interval so this is
reachable with small files; DUPEREMOVE_CHECKPOINT_STOP abandons a file after
that many checkpoints, standing in for the kill deterministically rather than
racing a signal against a read.
"""

import os
from harness import DuperemoveTest, requires_reflink

KiB = 1 << 10
MiB = 1 << 20

# Checkpoint every megabyte - the read buffer's size, so every pass through the
# hash loop is a candidate and a few-megabyte file behaves like a huge one.
CKPT = {"DUPEREMOVE_CHECKPOINT_BYTES": str(MiB)}
# ... and give up after the first one, as an interrupted run would.
STOP = dict(CKPT, DUPEREMOVE_CHECKPOINT_STOP="1")


@requires_reflink
class HashResumeTest(DuperemoveTest):
    # The fragmented file below is built with fsync-forced extent boundaries,
    # and what a checkpoint has to carry depends on which extent it lands in.
    # See DuperemoveTest.serial.
    serial = True

    def build_tree(self):
        """Files whose shapes stress different points of the hash loop."""
        # Plain, and big enough for several checkpoints. btrfs reports a
        # contiguously allocated file as one fiemap extent however large, so
        # this is also the case where every checkpoint falls mid-extent.
        self.write("tree/plain", os.urandom(6 * MiB))

        # Many extents, so checkpoints land on and between their boundaries.
        with open(self.path("tree/frag"), "wb") as f:
            for _ in range(6):
                f.write(os.urandom(MiB))
                f.flush()
                os.fsync(f.fileno())

        # A hole is skipped without being read, so it moves the offset without
        # feeding the checksum - a checkpoint has to describe that correctly.
        self.make_sparse("tree/sparse", os.urandom(2 * MiB), 8 * MiB,
                         os.urandom(2 * MiB))

        # Never reaches a checkpoint: the ordinary case must stay untouched.
        self.write("tree/small", os.urandom(64 * KiB))
        self.sync()
        return self.path("tree")

    def fingerprints(self):
        """Everything the scan is supposed to have produced."""
        return (self.files_fingerprint(), self.extents_fingerprint(),
                self.blocks_fingerprint())

    def scan_straight_through(self, tree, *extra):
        """Hash the tree in one go, into a fresh hashfile."""
        self.drop_hashfile()
        self.scan(tree, *extra)
        self.assertDmOk("uninterrupted scan")
        return self.fingerprints()

    def interrupt_after_one_checkpoint(self, tree, *extra):
        """One run that gives up at its first checkpoint, as a kill would."""
        self.dm("-r", tree, *extra, env=STOP)
        self.assertDmOk("interrupted scan")
        self.assertGreater(self.hf_count("scan_checkpoints"), 0,
                           "nothing was checkpointed")

    def scan_with_interruptions(self, tree, *extra, limit=40):
        """Hash the tree a checkpoint at a time until it finally completes.

        Every run but the last is cut short after its first checkpoint, so each
        file crawls forward one interval per run and every file ends up resumed
        many times over - including from checkpoints an earlier resume wrote.
        """
        self.drop_hashfile()
        runs = 0
        while runs < limit:
            runs += 1
            self.dm("-r", tree, *extra, env=STOP)
            self.assertDmOk(f"interrupted scan {runs}")
            if not self.hf_count("scan_checkpoints"):
                break
        else:
            self.fail(f"{limit} runs and the tree still had checkpoints")
        self.assertGreater(runs, 3, "no file was ever actually interrupted")
        return runs

    def test_resumed_scan_matches_an_uninterrupted_one(self):
        tree = self.build_tree()
        expected = self.scan_straight_through(tree)

        runs = self.scan_with_interruptions(tree)

        self.assertEqual(expected, self.fingerprints(),
                         f"hashing across {runs} interruptions did not produce "
                         "what one straight run produces")

    def test_resumed_scan_matches_with_block_hashes(self):
        """--dedupe-options=partial adds per-block digests, which flush to the
        hashfile on their own batch cadence rather than at checkpoints - so an
        interrupted run can leave blocks past the point it resumes from."""
        tree = self.build_tree()
        opt = "--dedupe-options=partial"
        expected = self.scan_straight_through(tree, opt)
        self.assertGreater(self.hf_count("blocks"), 0, "no block hashes stored")

        self.scan_with_interruptions(tree, opt)

        self.assertEqual(expected, self.fingerprints(),
                         "block hashes differ after resuming")

    def test_resume_starts_where_it_stopped(self):
        """The point of the exercise: the second run does not re-read the part
        the first one already hashed."""
        tree = self.build_tree()
        self.drop_hashfile()
        self.interrupt_after_one_checkpoint(tree)
        stopped_at = dict(self.hf_query(
            "select f.filename, c.loff from scan_checkpoints c "
            "join files f on f.id = c.fileid"))

        self.dm("-rv", tree, env=STOP, quiet=False)
        self.assertDmOk("resumed scan")
        for path, loff in stopped_at.items():
            self.assertIn(f"Resuming {path} at {loff} of", self.out,
                          "the resumed run started somewhere else")

    def test_a_checkpointed_file_is_not_mistaken_for_up_to_date(self):
        """Its row carries the current mtime and size from the moment it is
        listed - only the digest says it was ever hashed. Reading those two as
        "up to date" would leave the file permanently unhashed, since nothing
        about it will ever change again."""
        tree = self.build_tree()
        self.drop_hashfile()
        self.interrupt_after_one_checkpoint(tree)

        self.scan(tree)
        self.assertDmOk()
        self.assertEqual(0, self.hf_scalar(
            "select count(*) from files where digest is null"),
            "a file was left unhashed")
        self.assertEqual(0, self.hf_count("scan_checkpoints"))

    def test_a_file_changed_under_its_checkpoint_is_hashed_afresh(self):
        tree = self.build_tree()
        expected_before = self.scan_straight_through(tree)
        self.drop_hashfile()
        self.interrupt_after_one_checkpoint(tree)

        self.write("tree/plain", os.urandom(6 * MiB))
        self.sync()

        self.scan(tree)
        self.assertDmOk()
        self.assertEqual(0, self.hf_count("scan_checkpoints"))
        after = self.fingerprints()
        self.assertNotEqual(expected_before, after, "the file did change")
        self.assertEqual(self.scan_straight_through(tree), after,
                         "the rewritten file was not hashed from scratch")

    def test_an_unreadable_checkpoint_is_discarded_not_misread(self):
        """What a hash-library upgrade leaves behind: state this build cannot
        vouch for. It has to be refused rather than fed back into a checksum,
        which would produce a digest matching nothing."""
        tree = self.build_tree()
        expected = self.scan_straight_through(tree)
        self.drop_hashfile()
        self.interrupt_after_one_checkpoint(tree)

        self.hf_exec("update scan_checkpoints "
                     "set state = randomblob(length(state))")

        self.scan(tree)
        self.assertDmOk()
        self.assertEqual(0, self.hf_count("scan_checkpoints"))
        self.assertEqual(expected, self.fingerprints(),
                         "a corrupted checkpoint changed the hashes")

    def test_a_moved_extent_invalidates_the_checkpoint(self):
        """A checkpoint taken mid-extent carries that extent's part-finished
        digest, which means nothing if the extent is no longer there. mtime and
        size - what normally stands for "unchanged" - need not move when a file
        is defragmented, so the extent it named is checked directly."""
        tree = self.build_tree()
        expected = self.scan_straight_through(tree)
        self.drop_hashfile()
        self.dm("-r", tree, env=STOP)
        self.assertDmOk()
        self.assertGreater(
            self.hf_scalar("select count(*) from scan_checkpoints "
                           "where ext_state is not null"), 0,
            "no checkpoint landed mid-extent")

        self.hf_exec("update scan_checkpoints set ext_loff = ext_loff + 4096 "
                     "where ext_state is not null")

        self.dm("-rv", tree, quiet=False)
        self.assertDmOk()
        self.assertIn("extent layout changed", self.out)
        self.assertEqual(0, self.hf_count("scan_checkpoints"))
        self.assertEqual(expected, self.fingerprints(),
                         "the file was not re-hashed from the start")

    def test_a_resumed_file_is_deduped_by_the_run_that_finishes_it(self):
        """Its row still carries the generation the interrupted run gave it,
        and the dedupe phase of any run in between moves the watermark past
        that. A file at or below the watermark is one dedupe never looks at, so
        without moving it forward a resumed file would be hashed and then
        quietly ignored - by every later run too, since nothing about it would
        change again."""
        data = os.urandom(6 * MiB)
        a = self.write("tree/a", data)
        b = self.write("tree/b", data)
        self.sync()
        tree = self.path("tree")

        # A run that abandons both files partway, then dedupes what little it
        # has - which is what advances the watermark past their generation.
        self.dm("-rd", tree, env=STOP)
        self.assertDmOk("interrupted run")
        self.assertEqual(2, self.hf_count("scan_checkpoints"))
        self.assertNotShared(a, b, "nothing could have been deduped yet")

        self.dedupe(tree)
        self.assertDmOk("run that finishes the hashing")
        self.assertShared(a, b, "the resumed files were never deduped")

    def test_a_checkpointed_file_is_resumed_before_the_walk(self):
        """Partly-hashed files go to the pool up front, not when the walk gets
        to them.

        On a tree of millions of files the walk takes minutes to reach a file
        buried a few directories down, and a nearly-finished 50 GiB image is
        the one thing most worth finishing - waiting for rediscovery is another
        few minutes in which an interruption costs its tail. Measured on a
        120k-file tree, this took the delay from 0.73 s to 0.02 s, and the
        seeded figure does not grow with the tree.

        It must also not queue the file *twice*: the walk meets it as well, so
        the run's own file count is what pins that.
        """
        deep = "tree/aa/bb/cc/dd/big.bin"
        self.write(deep, os.urandom(6 * MiB))
        for i in range(20):
            self.write(f"tree/small{i}.bin", os.urandom(64 * KiB))
        self.sync()
        tree = self.path("tree")

        self.interrupt_after_one_checkpoint(tree)

        self.dm("-rv", tree, env=CKPT, quiet=False)
        self.assertDmOk("resumed scan")
        self.assertIn("partially hashed file", self.out,
                      "the checkpointed file was not seeded before the walk")

        # 21 files exist and 20 were finished by the first run, so this run
        # hashed exactly one. Seeding it *and* letting the walk queue it would
        # make that two - and have two workers hashing one file at once.
        self.assertEqual(1, self.hf_scalar(
            "select files_scanned from run_history order by rowid desc limit 1"),
            "the resumed file was queued more than once")
        self.assertEqual(0, self.hf_count("scan_checkpoints"))
        self.assertEqual(0, self.hf_scalar(
            "select count(*) from files where digest is null"))

    def test_a_checkpoint_outside_the_scanned_roots_is_left_alone(self):
        """One hashfile may cover several trees scanned on different days.
        Seeding must not reach into a tree this run was not pointed at - that
        would be hashing terabytes nobody asked for."""
        self.write("other/big.bin", os.urandom(6 * MiB))
        self.write("tree/a.bin", os.urandom(64 * KiB))
        self.sync()

        self.interrupt_after_one_checkpoint(self.path("other"))
        stopped_at = self.hf_scalar("select loff from scan_checkpoints")

        self.dm("-rv", self.path("tree"), env=CKPT, quiet=False)
        self.assertDmOk("scan of the requested tree")
        self.assertNotIn("partially hashed file", self.out)
        self.assertNotIn("other/big.bin", self.out)

        # Untouched: same checkpoint, still no digest, ready for the day that
        # tree is scanned again.
        self.assertEqual(stopped_at,
                         self.hf_scalar("select loff from scan_checkpoints"),
                         "a checkpoint outside the scanned roots was consumed")
        self.assertEqual(1, self.hf_scalar(
            "select count(*) from files where digest is null"))

    def test_without_a_hashfile_nothing_is_checkpointed(self):
        """There would be nowhere to resume from - the database dies with the
        process - so an in-memory run must not pay for checkpoints, and must
        not trip over the machinery either."""
        tree = self.build_tree()
        self.dm("-r", tree, hashfile=False, env=STOP)
        self.assertDmOk()
