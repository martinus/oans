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

from harness import REPO_DIR, DuperemoveTest

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

    def test_a_bad_option_value_exits_one(self):
        """#284: each of these crashed, ran with a value nobody asked for, or
        exited with a status the man page does not list."""
        tree = self._tree()
        for args in (("-B", "0"),              # SIGFPE after the scan
                     ("-B", "4G"),             # narrowed to 0: the same
                     ("--io-threads=-1",),     # 4294967295 workers, abort
                     ("--io-threads=8x",),     # taken as 8
                     ("--cpu-threads=0",),
                     ("-m", "10KB"),           # exit 51
                     ("-m", ""),               # exit 50
                     ("--max-filesize=16E",),  # wrapped to 0
                     ("-b", "4194308K")):      # 2^32 + 4K, taken as 4K
            self.dm("-r", *args, tree)
            self.assertEqual(1, self.rc, f"{args}: {self.out}")

    def test_a_bare_replay_of_a_missing_hashfile_creates_nothing(self):
        """#284: a typo in the path left an empty hashfile behind."""
        typo = os.path.join(self.work, "typo.db")
        self.dm("--hashfile", typo, hashfile=False)
        self.assertEqual(1, self.rc, self.out)
        self.assertFalse(os.path.exists(typo))

    def test_a_report_on_a_hashfile_that_cannot_be_opened_exits_one(self):
        for mode in ("--stats", "--history", "--json", "-L"):
            self.dm(mode, "--hashfile", "/nonexistent/dir/x.db",
                    hashfile=False)
            self.assertEqual(1, self.rc, f"{mode}: {self.out}")

    def test_an_interrupted_replay_that_lost_a_root_says_interrupted(self):
        """#284: the signal's status wins over "incomplete", as it does over
        success - a wrapper has to see that the run was stopped."""
        for i in range(4):
            self.mkrand(f"r1/x{i}.bin", 50000)
        self.mkrand("r2/y.bin", 50000)
        self.sync()
        self.dm("-r", self.path("r1"), self.path("r2"))
        self.assertDmOk()
        import shutil
        shutil.rmtree(self.path("r2"))
        for i in range(4):                     # something left to hash
            os.utime(self.path(f"r1/x{i}.bin"), (1, 1))
        self.dm(env={"DUPEREMOVE_INTERRUPT_AFTER": "1"})
        self.assertEqual(130, self.rc, self.out)

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



class UnitExitStatusTest(unittest.TestCase):
    """The shipped unit must read the statuses above the way they are meant.

    A stop or reboot mid-run exits 128 plus the signal; without those in
    SuccessExitStatus= systemd marked the unit failed and fired OnFailure= on
    every `systemctl stop`. Exit 2 must stay out of it, or a lost root would
    look healthy again.
    """

    def test_a_stop_is_success_and_a_lost_root_is_not(self):
        unit = os.path.join(REPO_DIR, "systemd", "oans@.service")
        codes = set()
        with open(unit) as f:
            for line in f:
                key, _, value = line.strip().partition("=")
                if key == "SuccessExitStatus":
                    codes.update(value.split())
        self.assertEqual({"130", "143"}, codes)
        self.assertNotIn(str(EXIT_INCOMPLETE), codes)


if __name__ == "__main__":
    unittest.main()
