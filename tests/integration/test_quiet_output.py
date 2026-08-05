"""-q must mean "errors and a one-line summary" (#148).

-q is the mode the shipped systemd unit runs, so it is the mode an unattended
job's journal is written from. It used to do close to the opposite of its
documentation: it kept the per-pass "Simple read and compare" headers (two per
generation pass, including ones reporting zero), suppressed the whole Summary
block -- the only place Reclaimed and the dedupe failure counters are printed --
and the single line it did emit was the fiemap net-change diagnostic, which
counts the surviving copy too and so reads ~1.5-2x the space actually freed.
"""

import re
import unittest

from harness import DuperemoveTest, requires_reflink

# The quiet one-liner, e.g. "Reclaimed 9.5 MiB across 1 group in 0.4s".
_QUIET_SUMMARY_RE = re.compile(
    r"^Reclaimed [\d.]+ [A-Za-z]+ across \d+ groups? in [\d.]+s$", re.M)
_PASS_HEADER = "Simple read and compare of file data found"


class TestQuietOutput(DuperemoveTest):
    def test_quiet_report_mode_prints_no_per_pass_headers(self):
        """Without -d, -q should say nothing at all about what it found."""
        self.mkdup("tree/a.bin", "tree/b.bin", 1 << 20)
        self.scan(self.path("tree"))
        self.assertDmOk()
        self.assertNotIn(_PASS_HEADER, self.out,
                         "-q kept the per-pass report header")

    def test_non_quiet_report_mode_still_describes_what_it_found(self):
        """The header is suppressed by -q, not removed: -r alone still prints it."""
        self.mkdup("tree/a.bin", "tree/b.bin", 1 << 20)
        self.dm("-r", self.path("tree"), quiet=False)
        self.assertDmOk()
        self.assertIn(_PASS_HEADER, self.out,
                      "non-quiet report mode lost its header")

    @requires_reflink
    def test_quiet_dedupe_prints_the_reclaimed_one_liner(self):
        """-q reports the honest Reclaimed figure, not just the fiemap delta."""
        self.mkdup("tree/a.bin", "tree/b.bin", 1 << 20)
        self.dedupe(self.path("tree"))
        self.assertDmOk()
        self.assertRegex(self.out, _QUIET_SUMMARY_RE,
                         "-q did not print the one-line summary")
        self.assertNotIn(_PASS_HEADER, self.out)

    @requires_reflink
    def test_quiet_reclaimed_matches_the_non_quiet_summary(self):
        """The -q figure is the same number the full Summary block reports.

        The bug this pins: -q used to print only the net-change diagnostic,
        which counts the surviving copy as well and is therefore roughly
        double for a pair. Whatever -q prints must be what a human reading
        the non-quiet summary would see, or the journal is misleading.
        """
        self.mkdup("tree/a.bin", "tree/b.bin", 1 << 20)
        self.dedupe(self.path("tree"))
        self.assertDmOk()
        quiet_figure = self.reclaimed_summary()
        self.assertIsNotNone(quiet_figure, "-q printed no Reclaimed figure")

        # Same tree, deduped from scratch, with the full human summary.
        self.mkdup("tree2/a.bin", "tree2/b.bin", 1 << 20)
        self.dm("-rd", self.path("tree2"), quiet=False)
        self.assertDmOk()
        self.assertEqual(quiet_figure, self.reclaimed_summary(),
                         "-q and the Summary block disagree on Reclaimed")

    @requires_reflink
    def test_quiet_still_reports_a_no_op_run(self):
        """A scheduled job that found nothing must still say so, in one line."""
        self.mkrand("tree/lonely.bin", 1 << 20)
        self.dedupe(self.path("tree"))
        self.assertDmOk()
        self.assertIn("Nothing to deduplicate", self.out)

    @requires_reflink
    def test_quiet_keeps_the_machine_readable_net_change_line(self):
        """Piped output keeps the net-change line: scripts grep for it.

        -q on a *terminal* no longer prints it (the Reclaimed line is the
        honest figure for a human), but the test harness captures a pipe,
        which is also what a journal capture and `oans -qd ... | grep` are.
        """
        self.mkdup("tree/a.bin", "tree/b.bin", 1 << 20)
        self.dedupe(self.path("tree"))
        self.assertDmOk()
        self.assertIsNotNone(self.net_change(),
                             "piped -q lost the net-change line")


if __name__ == "__main__":
    unittest.main()
