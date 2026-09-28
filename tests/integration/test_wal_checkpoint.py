"""A scan must not pin the WAL once the walk is over (#261).

A scan of one 8 TB image kept a read transaction open from its last listed
file until the hashing ended, so no checkpoint could copy the WAL and it grew
to 513 GB. The test holds a run in the middle of one file with
DUPEREMOVE_CHECKPOINT_PAUSE, which stops the process at a named hash checkpoint:
a timed poll would race the hashing, and a fast disk finishes the file first.
The hook waits for the walk to end before it stops the run, and every write so
far is committed, so a checkpoint from outside must copy all of it.
"""

import os
import signal
import subprocess

from harness import (DUPEREMOVE, DuperemoveTest, _settle_scratch,
                     requires_real_binary, requires_reflink)

MiB = 1 << 20


@requires_reflink
class WalCheckpointTest(DuperemoveTest):
    @requires_real_binary   # the hook stops oans, not a wrapper around it
    def test_the_scan_does_not_pin_the_wal_while_it_hashes(self):
        self.mkrand("tree/image", 4 * MiB)
        _settle_scratch()   # what dm() does: extents must exist to be hashed

        # A hash checkpoint every megabyte force-commits the write batch; the
        # 4K block hashes of partial mode give each commit frames to copy.
        env = dict(os.environ,
                   DUPEREMOVE_CHECKPOINT_BYTES=str(MiB),
                   DUPEREMOVE_CHECKPOINT_PAUSE="3")
        p = subprocess.Popen(
            [DUPEREMOVE, "-q", "--io-threads=1", "-b", "4096",
             "--dedupe-options=partial", "--hashfile", self.hf,
             "-r", self.path("tree")],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=env)
        try:
            _, status = os.waitpid(p.pid, os.WUNTRACED)
            self.assertTrue(os.WIFSTOPPED(status),
                            "the run ended before its 3rd hash checkpoint")
            busy, log, done = self.hf_query("PRAGMA wal_checkpoint(PASSIVE)")[0]
        finally:
            if p.poll() is None:
                os.kill(p.pid, signal.SIGCONT)
        self.assertEqual(0, p.wait(timeout=60), "the resumed run failed")

        self.assertGreater(log, 0, "nothing was committed to the WAL")
        self.assertEqual(log, done,
                         f"only {done} of {log} WAL frames could be "
                         f"checkpointed: an open read transaction pins them")
        self.assertEqual(0, busy)
