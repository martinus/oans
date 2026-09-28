"""A replayed config with no -d must say so (#149).

An oans@.timer job can run every week forever, exit 0 every time, and never
deduplicate anything -- because the run that seeded its hashfile was missing
-d. The replay mechanism is working as designed; the problem is that the setup
flow leads users into it (the docs walk through a first run to establish the
hashfile, and the man page actively suggests a preview run without -d), and the
resulting job is indistinguishable from a healthy one: it walks the tree,
updates hashes, records a run, exits 0, and reclaims nothing. --history shows a
growing list of runs with 0 B reclaimed, which is also exactly what a healthy
job on an already-deduped tree looks like.

Not an error: a hash-only scheduled job is a legitimate setup, so this warns
rather than refuses.
"""

import json
import subprocess
import shlex
import unittest

from harness import DUPEREMOVE, DuperemoveTest, requires_reflink

WARNING = "has no -d"


class ReplayNoDedupeTest(DuperemoveTest):
    def _tree(self):
        self.mkdup("tree/a.bin", "tree/b.bin", 100000)
        self.sync()
        return self.path("tree")

    def test_replay_of_a_scan_only_config_warns(self):
        self.dm("-r", self._tree())          # seeded WITHOUT -d
        self.assertDmOk()

        self.dm()                            # bare replay
        self.assertIn(WARNING, self.out,
                      "a timer that never dedupes gave no indication")

    def test_the_warning_names_the_command_that_fixes_it(self):
        """The point of a replay is that the user forgot the arguments."""
        tree = self._tree()
        self.dm("-r", tree)
        self.dm()
        self.assertIn("-r -d", self.out, "no fix-up command offered")
        self.assertIn(self.hf, self.out, "fix-up command omits the hashfile")
        self.assertIn(tree, self.out, "fix-up command omits the stored path")

    @requires_reflink
    def test_the_command_it_prints_keeps_every_stored_setting(self):
        """#285: the fix-up is a normal run, so it stores its own config.
        It used to name only -r, -d and the roots, so running it erased the
        stored excludes and size limits - and from then on the timer scanned
        and deduplicated exactly what the job was set up to skip."""
        tree = self._tree()
        self.dm("-r", "--exclude=u2", "--min-filesize=2K", "--skip-zeroes",
                "--dedupe-options=nosame", tree)
        before = self.stored()
        self.dm()
        line = next(l for l in self.out.splitlines()
                    if l.strip().startswith("oans "))
        argv = shlex.split(line.strip())
        argv[0] = DUPEREMOVE
        proc = subprocess.run(argv, stdout=subprocess.PIPE,
                              stderr=subprocess.STDOUT, text=True)
        self.assertEqual(0, proc.returncode, proc.stdout)
        after = self.stored()
        self.assertEqual(before[1:], after[1:], "a stored setting was lost")
        self.assertEqual(1, after[0], "and -d is stored now")

    def stored(self):
        return (self.hf_scalar("select keyval from config "
                               "where keyname = 'opt_run_dedupe'"),
                self.hf_query("select * from scan_excludes"),
                self.hf_query("select keyname, keyval from config "
                              "where keyname like 'opt_%' "
                              "and keyname <> 'opt_run_dedupe' "
                              "order by keyname"))

    def test_the_warning_survives_quiet(self):
        """-q is what the shipped systemd unit runs, so it must show there."""
        self.dm("-r", self._tree())
        self.dm(quiet=True)
        self.assertIn(WARNING, self.out)

    @requires_reflink
    def test_a_dedupe_config_replays_silently(self):
        """No false positive on the healthy setup."""
        self.dm("-rd", self._tree())
        self.assertDmOk()
        self.dm()
        self.assertDmOk()
        self.assertNotIn(WARNING, self.out,
                         "warned about a config that does deduplicate")

    def test_an_explicit_scan_only_run_does_not_warn(self):
        """Only a replay is silent about its own options; a command line is not.

        Someone who types `oans -r <path>` can see there is no -d. Warning
        there would fire on every ordinary preview run.
        """
        self.dm("-r", self._tree())
        self.assertNotIn(WARNING, self.out)

    def test_json_exposes_the_configured_mode(self):
        """A dashboard should be able to spot the read-only job."""
        self.dm("-r", self._tree())
        metrics = json.loads(self.dm("--json"))
        self.assertIs(False, metrics["scan_configured_dedupe"])

    @requires_reflink
    def test_json_reports_a_dedupe_config_as_such(self):
        self.dm("-rd", self._tree())
        metrics = json.loads(self.dm("--json"))
        self.assertIs(True, metrics["scan_configured_dedupe"])

    @requires_reflink
    def test_the_suggested_command_actually_fixes_it(self):
        """End to end: follow the advice, and the warning stops."""
        tree = self._tree()
        self.dm("-r", tree)
        self.dm()
        self.assertIn(WARNING, self.out)

        # What the warning tells the user to run.
        self.dm("-rd", tree)
        self.assertDmOk()

        self.dm()
        self.assertNotIn(WARNING, self.out,
                         "following the suggested command did not fix it")


if __name__ == "__main__":
    unittest.main()
