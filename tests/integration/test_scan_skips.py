"""Scan-phase skips must be counted, reported and exported (#145).

oans is built for unattended scheduled runs, but the scan phase used to count
only what it succeeded at: every "I skipped this" path wrote a line to stderr
and was then forgotten. A weekly timer that lost /srv/media/photos to a
permissions change months ago looked identical to a healthy one -- nothing in
the summary, --json or --history said otherwise.

The buckets are split into problems (permission, unreadable, path_too_long,
unsupported_fs) and the user's own configuration (excluded, too_small,
not_regular). The split is the point: "812 skipped" reads as alarming when 812
of them are --exclude patterns doing their job.
"""

import json
import os
import stat
import unittest

from harness import DuperemoveTest


class ScanSkipsTest(DuperemoveTest):
    def _unreadable_subtree(self):
        """A tree with one subdirectory the scanner cannot open."""
        self.mkdup("tree/a.bin", "tree/b.bin", 200000)
        sub = self.path("tree/sub/c.bin")
        self.mkrand("tree/sub/c.bin", 200000)
        os.chmod(os.path.dirname(sub), 0o000)
        return os.path.dirname(sub)

    def tearDown(self):
        # Restore any 000 directory so rmtree can clean up.
        for root, dirs, _files in os.walk(self.work):
            for d in dirs:
                p = os.path.join(root, d)
                try:
                    os.chmod(p, stat.S_IRWXU)
                except OSError:
                    pass
        super().tearDown()

    def test_permission_skip_is_reported_under_quiet(self):
        """The failure an unattended journal must carry survives -q."""
        self._unreadable_subtree()
        self.scan(self.path("tree"))
        self.assertIn("Skipped", self.out,
                      "an unreadable subtree left no trace in the summary")
        self.assertIn("permission denied", self.out)

    def test_permission_skip_reaches_json(self):
        self._unreadable_subtree()
        self.scan(self.path("tree"))

        out = self.dm("--json", hashfile=True)
        metrics = json.loads(out)
        self.assertEqual(1, metrics["scan_skipped_last_run"]["permission"])
        self.assertEqual(1, metrics["scan_skipped_errors_total"])

    def test_clean_run_reports_no_skips(self):
        """No false positives: a healthy tree must stay silent."""
        self.mkdup("tree/a.bin", "tree/b.bin", 200000)
        self.scan(self.path("tree"))
        self.assertNotIn("Skipped", self.out)

        metrics = json.loads(self.dm("--json"))
        for bucket, n in metrics["scan_skipped_last_run"].items():
            self.assertEqual(0, n, f"clean run reported {bucket}={n}")
        self.assertEqual(0, metrics["scan_skipped_errors_total"])

    def test_config_skips_are_not_reported_as_errors(self):
        """--exclude hits are the user's own choice, not a degraded run.

        This is the distinction that makes the number actionable: an operator
        alarming on scan_skipped_errors_total must not be woken by their own
        exclude patterns matching, however many files they match.
        """
        self.mkdup("tree/a.bin", "tree/b.bin", 200000)
        self.mkrand("tree/skipme/c.bin", 200000)
        self.mkrand("tree/skipme/d.bin", 200000)
        self.scan(self.path("tree"), "--exclude", "skipme/")

        self.assertNotIn("Skipped", self.out,
                         "--exclude hits were reported as errors")
        metrics = json.loads(self.dm("--json"))
        self.assertEqual(0, metrics["scan_skipped_errors_total"])

    def test_history_marks_a_degraded_run(self):
        """--history must distinguish 'reclaimed nothing' from 'lost a subtree'.

        Both print 0 B, which is exactly why the skip count has to be on the
        timeline row rather than only in the JSON.
        """
        self._unreadable_subtree()
        self.scan(self.path("tree"))

        out = self.dm("--history")
        self.assertIn("skipped", out,
                      "--history did not flag the degraded run")

    def test_history_columns_survive_an_older_hashfile(self):
        """The new columns are additive: an existing hashfile is migrated.

        Per CLAUDE.md this must not bump DB_FILE_MINOR -- a bump would discard
        every user's hashfile and force a full re-scan for a reporting change.
        So a hashfile whose run_history predates the columns must be brought up
        by ALTER TABLE, keeping its rows.
        """
        self.mkdup("tree/a.bin", "tree/b.bin", 200000)
        self.scan(self.path("tree"))
        runs_before = self.hf_scalar("select count(*) from run_history")
        self.assertEqual(1, runs_before)

        # Simulate the pre-#145 schema by dropping the columns back off.
        con = __import__("sqlite3").connect(self.hf)
        try:
            for col in ("skip_permission", "skip_unreadable",
                        "skip_path_too_long", "skip_unsupported_fs"):
                con.execute(f"alter table run_history drop column {col}")
            con.commit()
        finally:
            con.close()

        # A later run must migrate rather than fail or discard the history.
        self.scan(self.path("tree"))
        self.assertDmOk()
        self.assertEqual(2, self.hf_scalar("select count(*) from run_history"),
                         "migrating the schema lost the earlier run")
        self.assertEqual(
            0, self.hf_scalar("select skip_permission from run_history "
                              "order by ts limit 1"),
            "the back-filled column should default to 0")


if __name__ == "__main__":
    unittest.main()
