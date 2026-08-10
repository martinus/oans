"""SIGINT/SIGTERM must commit the open write batch, not throw it away (#201).

Scan durability rides on the batched writer, which commits every
COMMIT_INTERVAL_SEC (10 s). Before this, a signal killed the process outright:
a run interrupted more often than that persisted *nothing*, over and over, and
`systemctl stop oans@...` - the scheduled NAS case - is exactly such a signal.

Racing a real `sleep N; kill` against a scan is not a test: to make the signal
reliably land mid-run the tree has to be big enough that even fast storage
cannot finish it, which means writing gigabytes in setUp and still coin-flipping
on a faster box. So the deterministic cases use DUPEREMOVE_INTERRUPT_AFTER,
which raises the *real* signal after a named number of files - same handler,
same unwinding, same exit status - exactly as DUPEREMOVE_CHECKPOINT_STOP stands
in for the kill in test_hash_resume.py. One test does send a real signal, to
keep the end-to-end path honest.
"""

import os
import signal
import subprocess
import time

from harness import DUPEREMOVE, DuperemoveTest, requires_reflink

KiB = 1 << 10

# The hook counts files that have *finished hashing*, so STOP_AFTER of them are
# in the open batch when the signal lands - that is the exact figure the flush
# either saves or loses. The rest leave the run genuinely cut short.
FILES = 300
STOP_AFTER = 200


class SignalFlushTest(DuperemoveTest):
    def build_tree(self):
        for i in range(FILES):
            self.write(f"tree/f{i:04d}.bin", os.urandom(16 * KiB))
        self.sync()
        return self.path("tree")

    def fingerprints(self):
        return (self.files_fingerprint(), self.extents_fingerprint(),
                self.blocks_fingerprint())

    def interrupt_scan(self, tree, sig="INT", *extra):
        env = {"DUPEREMOVE_INTERRUPT_AFTER": str(STOP_AFTER)}
        if sig != "INT":
            env["DUPEREMOVE_INTERRUPT_SIGNAL"] = sig
        self.dm("-r", tree, *extra, env=env)

    def test_sigint_persists_what_was_hashed(self):
        tree = self.build_tree()
        self.interrupt_scan(tree)

        # 128+SIGINT, what a shell reports for a signalled child.
        self.assertEqual(130, self.rc, self.out)
        # The regression: on v1.10.1 this is 0, because the batch that held
        # every one of these rows was discarded with the process.
        hashed = self.hf_scalar(
            "select count(*) from files where digest is not null")
        self.assertGreaterEqual(hashed, STOP_AFTER,
                                "the open write batch was discarded")
        self.assertLess(hashed, FILES, "the run was not actually cut short")

    def test_sigterm_persists_what_was_hashed(self):
        tree = self.build_tree()
        self.interrupt_scan(tree, "TERM")

        self.assertEqual(143, self.rc, self.out)
        self.assertGreaterEqual(
            self.hf_scalar("select count(*) from files where digest is not null"),
            STOP_AFTER)

    def test_an_interrupted_run_is_not_recorded_as_a_completed_one(self):
        """--history must not show a partial scan as a finished one."""
        tree = self.build_tree()
        self.interrupt_scan(tree)
        self.assertEqual(0, self.hf_count("run_history"))

        # The run that does finish is recorded, so nothing is lost either.
        self.scan(tree)
        self.assertDmOk()
        self.assertEqual(1, self.hf_count("run_history"))

    def test_resuming_matches_one_straight_through_scan(self):
        """The property that matters: an interrupted-then-resumed hashfile is
        indistinguishable from one produced in a single run.

        A wrong digest is invisible downstream - it just looks like a file with
        no duplicate - so nothing but a byte-for-byte comparison would catch a
        flush that dropped or garbled a row.
        """
        tree = self.build_tree()
        opts = ("-b", "4096", "--dedupe-options=partial")

        self.scan(tree, *opts)
        self.assertDmOk("uninterrupted scan")
        expected = self.fingerprints()

        self.drop_hashfile()
        self.interrupt_scan(tree, "INT", *opts)
        self.assertEqual(130, self.rc, self.out)
        partial = self.hf_scalar(
            "select count(*) from files where digest is not null")
        self.assertGreater(partial, 0)

        self.scan(tree, *opts)
        self.assertDmOk("resumed scan")
        self.assertEqual(expected, self.fingerprints())

    @requires_reflink
    def test_interrupting_the_dedupe_phase_leaves_a_usable_hashfile(self):
        """Batches in flight finish; no further ones start.

        The generation-ordered watermark means dedupe_seq still names only
        fully-processed generations, so the follow-up run picks up the rest and
        the one after that converges - reclaiming nothing, the invariant #186
        exists to protect.
        """
        for i in range(40):
            self.mkdup(f"tree/a{i}.bin", f"tree/b{i}.bin", 128 * KiB)
        self.sync()
        tree = self.path("tree")

        # Many small generations, so there are batches left to cut off.
        env = {"DUPEREMOVE_FILES_PER_PASS": "2",
               "DUPEREMOVE_INTERRUPT_AFTER_BATCHES": "1"}
        self.dm("-rd", "-B", "1", tree, env=env)
        self.assertEqual(130, self.rc, self.out)

        # The generations the interrupted run never reached are still pending,
        # so the follow-up has real work: proof the watermark did not run ahead
        # of what was actually deduped.
        self.dm("-rd", tree, quiet=False)
        self.assertDmOk("run after an interrupted dedupe phase")
        summary = self.reclaimed_summary()
        self.assertTrue(summary and summary[0] != "0 B",
                        "the interrupted run advanced dedupe_seq past "
                        f"generations it never processed; got {summary}")

        # And the tree has converged.
        self.dm("-rd", tree, quiet=False)
        self.assertDmOk("convergence run")
        self.assertReclaimedNothing("the tree is already deduped")

    def test_a_real_signal_is_handled_like_the_hook(self):
        """The hook raises a real signal, but nothing beats sending one.

        Scoped to what an external signal can guarantee on unknown hardware:
        that it is *caught* - the run exits 130 rather than dying on the
        default action - and that it exits promptly. How much got hashed first
        is a race with the disk, so the durability claim is left to the
        hook-driven tests above, which pin it exactly.
        """
        # Under make integration-valgrind, DUPEREMOVE is a shell wrapper: the
        # signal would reach the script (which dies on it, exit -2), not oans.
        # The hook-driven tests above do run there, under memcheck.
        with open(DUPEREMOVE, "rb") as f:
            if f.read(2) == b"#!":
                self.skipTest("DUPEREMOVE is a wrapper script, not the binary")

        for i in range(2000):
            self.write(f"tree/f{i:04d}.bin", os.urandom(64 * KiB))
        self.sync()

        p = subprocess.Popen(
            [DUPEREMOVE, "-q", "--io-threads=1", "-b", "4096",
             "--dedupe-options=partial", "--hashfile", self.hf,
             "-r", self.path("tree")],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        time.sleep(0.1)
        try:
            p.send_signal(signal.SIGINT)
        except ProcessLookupError:
            pass
        rc = p.wait(timeout=60)

        if rc == 0:
            self.skipTest("the scan finished before the signal was sent")
        # Not -2: a negative code means the default action killed it, which is
        # the behaviour this whole change replaces.
        self.assertEqual(130, rc, "a real SIGINT was not handled")
