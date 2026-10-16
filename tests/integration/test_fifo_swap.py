"""A file swapped for a FIFO after the walk listed it must not hang the run.

The walker stats each entry and queues the regular files; hashing opens them
later. A FIFO put at the name in between used to be opened O_RDONLY, which
waits for a writer that never comes - and with SA_RESTART the first Ctrl-C
restarted the open. The test holds a run in the middle of one file with
DUPEREMOVE_CHECKPOINT_PAUSE, once the walk has queued the other one, swaps
that other file for a FIFO, and lets the run go on: it must finish, and skip
the FIFO.
"""

import os
import signal
import subprocess

from harness import (DUPEREMOVE, DuperemoveTest, _settle_scratch,
                     skip_without_hooks,
                     requires_real_binary, requires_reflink)

MiB = 1 << 20


@requires_reflink
class FifoSwapTest(DuperemoveTest):
    @requires_real_binary   # the hook stops oans, not a wrapper around it
    def test_a_queued_file_swapped_for_a_fifo_is_skipped(self):
        self.mkrand("tree/a", 4 * MiB)
        self.mkrand("tree/b", 4 * MiB)
        _settle_scratch()

        # One csum worker, so the file not being hashed is still queued.
        env = dict(os.environ,
                   DUPEREMOVE_CHECKPOINT_BYTES=str(MiB),
                   DUPEREMOVE_CHECKPOINT_PAUSE="1")
        skip_without_hooks(env)
        p = subprocess.Popen(
            [DUPEREMOVE, "-v", "--io-threads=1", "--hashfile", self.hf,
             "-r", self.path("tree")],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env)
        try:
            _, status = os.waitpid(p.pid, os.WUNTRACED)
            self.assertTrue(os.WIFSTOPPED(status),
                            "the run ended before its first hash checkpoint")
            busy = self.hf_query("SELECT f.filename FROM scan_checkpoints c "
                                 "JOIN files f ON f.id = c.fileid")
            self.assertEqual(1, len(busy), busy)
            other = self.path("tree/b" if busy[0][0].endswith("/a")
                              else "tree/a")
            os.unlink(other)
            os.mkfifo(other)
            os.kill(p.pid, signal.SIGCONT)
            try:
                out, _ = p.communicate(timeout=30)
            except subprocess.TimeoutExpired:
                self.fail("the run hung opening the FIFO")
        finally:
            if p.poll() is None:
                p.kill()
                p.wait()

        self.assertEqual(0, p.returncode, out.decode(errors="replace"))
        rows = dict(self.hf_query("SELECT filename, digest IS NOT NULL "
                                  "FROM files"))
        self.assertEqual(1, rows.get(busy[0][0]),
                         "the file being hashed was not finished")
        self.assertNotEqual(1, rows.get(other),
                            "the FIFO was stored with a digest")
