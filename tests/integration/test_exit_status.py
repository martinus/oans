"""A run that could not cover what it was given must say so in $? (#146).

oans exited 0 when a path named on the command line could not be resolved, and
when a replayed scan config had lost a root. For the scheduled deployment that
is the difference between a monitored job and an unmonitored one: systemd marks
the unit SUCCESS, OnFailure= never fires, and `if oans ...; then` takes the
happy path. The shipped oans@.service is Type=oneshot with no OnFailure=, so
there was no configuration of it that could notice.

Exit 2 means "completed, but covered less than asked", leaving 1 for a real
failure so a wrapper can tell degraded from broken without parsing --json.

Deliberately NOT covered by exit 2: skips the user asked for (--exclude,
--min-filesize, a non-regular file), and files met mid-walk that could not be
read -- a real NAS tree routinely contains a handful of those, and failing
every run over one unreadable lock file is the wrong default. That is the
broader --strict question, left open on the issue.
"""

import os
import stat
import unittest

from harness import DuperemoveTest

EXIT_INCOMPLETE = 2


class ExitStatusTest(DuperemoveTest):
    def tearDown(self):
        for root, dirs, _files in os.walk(self.work):
            for d in dirs:
                try:
                    os.chmod(os.path.join(root, d), stat.S_IRWXU)
                except OSError:
                    pass
        super().tearDown()

    def _tree(self):
        self.mkdup("tree/a.bin", "tree/b.bin", 100000)
        self.sync()
        return self.path("tree")

    # -- the cases that must fail ------------------------------------------

    def test_nonexistent_root_exits_incomplete(self):
        self._tree()
        self.dm("-r", self.path("nope"))
        self.assertEqual(EXIT_INCOMPLETE, self.rc)

    def test_one_bad_root_among_good_ones_still_scans_the_good_ones(self):
        """Degraded, not aborted: the rest of the tree is still processed."""
        tree = self._tree()
        self.dm("-r", tree, self.path("nope"))
        self.assertEqual(EXIT_INCOMPLETE, self.rc)
        self.assertEqual(2, self.hf_count("files"),
                         "a bad root stopped the good one being scanned")

    def test_quoted_glob_exits_incomplete(self):
        """The issue's reproduction: an accidentally-quoted shell glob."""
        tree = self._tree()
        self.dm("-r", os.path.join(tree, "*"))
        self.assertEqual(EXIT_INCOMPLETE, self.rc)

    def test_replayed_config_that_lost_a_root_exits_incomplete(self):
        """The scheduled-job case: an unmounted share narrows every run."""
        self.mkrand("r1/x.bin", 50000)
        self.mkrand("r2/y.bin", 50000)
        self.sync()
        self.dm("-r", self.path("r1"), self.path("r2"))
        self.assertDmOk()

        # Bare replay while both roots exist: healthy.
        self.dm()
        self.assertEqual(0, self.rc)

        import shutil
        shutil.rmtree(self.path("r2"))
        self.dm()
        self.assertEqual(EXIT_INCOMPLETE, self.rc,
                         "a replay that lost a root reported success")

    # -- the cases that must NOT fail --------------------------------------

    def test_clean_run_exits_zero(self):
        self.dm("-r", self._tree())
        self.assertEqual(0, self.rc)

    def test_excluded_root_exits_zero(self):
        """The user asked for this skip; it is not a failure."""
        tree = self._tree()
        self.dm("-r", tree, "--exclude", "tree/")
        self.assertEqual(0, self.rc)

    def test_root_below_min_filesize_exits_zero(self):
        self.write("tiny.bin", b"x" * 10)
        self.sync()
        self.dm("-r", self.path("tiny.bin"), "--min-filesize=1M")
        self.assertEqual(0, self.rc)

    def test_non_regular_root_exits_zero(self):
        os.symlink("/dev/null", self.path("link"))
        self.dm("-r", self.path("link"))
        self.assertEqual(0, self.rc)

    def test_unreadable_directory_mid_walk_still_exits_zero(self):
        """Out of scope on purpose: that is the broader --strict question.

        A NAS tree routinely holds a few files the scanner cannot open, so
        failing the whole run over one is the wrong default. #145's counters
        already make these visible; only the exit status is left alone.
        """
        self.mkrand("tree2/ok.bin", 1000)
        self.mkrand("tree2/locked/x.bin", 1000)
        os.chmod(self.path("tree2/locked"), 0o000)
        self.sync()
        self.dm("-r", self.path("tree2"))
        self.assertEqual(0, self.rc)
        # ...but it is still reported, per #145.
        self.assertIn("Skipped", self.out)

    def test_replay_with_all_roots_gone_still_fails_hard(self):
        """Unchanged: that refuses to run at all, so it keeps exit 1.

        Scanning zero roots would let the stat-based prune wipe the hashfile,
        so this must stay a hard failure and not be softened to 'incomplete'.
        """
        self.mkrand("r1/x.bin", 50000)
        self.sync()
        self.dm("-r", self.path("r1"))
        self.assertDmOk()

        import shutil
        shutil.rmtree(self.path("r1"))
        self.dm()
        self.assertEqual(1, self.rc)
        self.assertGreater(self.hf_count("files"), 0,
                           "refusing to run must not have pruned the hashfile")


if __name__ == "__main__":
    unittest.main()
