"""Interrupting a scan, and picking it up again (#281).

Four ways the #201 interrupt handling and the #159 resume fell short:

- a worker noticed Ctrl-C only at a checkpoint - once per GiB, and never
  without a hashfile - so a large file was read to its end first;
- checkpointed files were seeded from a prefix match on the root, which
  ignores --exclude (it works by the walk never entering a directory), -r
  and symlinks;
- a checkpoint written without extent state (only_whole_files) was resumed by
  a run that hashes extents, and the extent's digest covered only its tail;
- a FIFO named as a root blocked the first open() until a writer came.
"""

import os
from harness import DuperemoveTest, requires_reflink

KiB, MiB = 1 << 10, 1 << 20
CKPT = {"DUPEREMOVE_CHECKPOINT_BYTES": str(MiB)}
STOP = dict(CKPT, DUPEREMOVE_CHECKPOINT_STOP="1")


@requires_reflink
class InterruptResumeTest(DuperemoveTest):
    serial = True       # extent layouts, as in test_hash_resume.py

    def test_ctrl_c_stops_a_large_file_within_a_buffer(self):
        """The small file finishes first and raises SIGINT; the large one is
        mid-read. At the default 1 GiB interval it used to be read to its end
        and stored with a digest; now it stops and checkpoints where it is."""
        big = self.write("big", os.urandom(128 * MiB))
        small = self.write("small", os.urandom(64 * KiB))
        self.sync()
        # Roots are queued in argument order, so big is taken first.
        self.dm(big, small, env={"DUPEREMOVE_INTERRUPT_AFTER": "1"})
        self.assertEqual(130, self.rc, self.out)
        row = self.hf_query(
            "select f.digest is null, c.loff from files f "
            "join scan_checkpoints c on c.fileid = f.id "
            "where f.filename like '%/big'")
        self.assertEqual(1, len(row), "big stopped with a checkpoint")
        unhashed, stopped_at = row[0]
        self.assertTrue(unhashed)
        self.assertLess(stopped_at, 128 * MiB)

    def interrupted(self, path):
        """A run over `path`'s tree that gives up at the first checkpoint."""
        self.dm("-r", self.path("data"), env=STOP)
        self.assertEqual(0, self.rc, self.out)
        self.assertEqual(1, self.hf_scalar(
            "select count(*) from scan_checkpoints c join files f "
            f"on f.id = c.fileid where f.filename like '%/{path}'"))

    def assertNotResumed(self, *args):
        before = self.hf_scalar("select loff from scan_checkpoints")
        self.dm("-v", *args, env=CKPT, quiet=False)
        self.assertEqual(0, self.rc, self.out)
        self.assertNotIn("partially hashed file", self.out)
        self.assertEqual(before,
                         self.hf_scalar("select loff from scan_checkpoints"),
                         "the checkpoint was consumed")

    def test_an_excluded_directory_is_not_seeded(self):
        self.write("data/vm/big.img", os.urandom(6 * MiB))
        self.write("data/a", os.urandom(64 * KiB))
        self.sync()
        self.interrupted("vm/big.img")
        self.assertNotResumed("-r", "--exclude", "vm", self.path("data"))

    def test_without_r_a_file_in_a_subdirectory_is_not_seeded(self):
        self.write("data/vm/big.img", os.urandom(6 * MiB))
        self.write("data/a", os.urandom(64 * KiB))
        self.sync()
        self.interrupted("vm/big.img")
        self.assertNotResumed(self.path("data"))

    def test_a_directory_replaced_by_a_symlink_is_not_seeded(self):
        self.write("data/vm/big.img", os.urandom(6 * MiB))
        self.sync()
        self.interrupted("vm/big.img")
        os.rename(self.path("data/vm"), self.path("elsewhere"))
        os.symlink(self.path("elsewhere"), self.path("data/vm"))
        self.assertNotResumed("-r", self.path("data"))

    def test_a_checkpoint_without_extent_state_is_not_resumed_into_one(self):
        """only_whole_files keeps no extent digest in a checkpoint. A default
        run resuming it hashed the extent from the checkpoint on, and stored
        that as the extent's digest."""
        self.write("data/f", os.urandom(8 * MiB))
        self.sync()
        tree = self.path("data")
        self.dm("-r", tree, "--dedupe-options=only_whole_files", env=STOP)
        self.assertEqual(0, self.rc, self.out)
        self.assertEqual(1, self.hf_count("scan_checkpoints"))

        self.dm("-r", tree, env=CKPT)
        self.assertEqual(0, self.rc, self.out)
        resumed = self.extents_fingerprint()
        self.drop_hashfile()
        self.dm("-r", tree)
        self.assertEqual(0, self.rc, self.out)
        self.assertEqual(self.extents_fingerprint(), resumed)

    def test_a_fifo_root_does_not_block(self):
        fifo = self.path("fifo")
        os.mkfifo(fifo)
        self.dm("-r", fifo, timeout=30)
