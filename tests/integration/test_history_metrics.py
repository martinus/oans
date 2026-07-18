"""Run history (recorded in the hashfile) and the machine-readable --json export.

Each run appends a row to run_history; `--history` prints the timeline and
lifetime totals; `--json` emits a flat metrics object (current stats + history
totals) for dashboards.
"""

import json
import os

from harness import DuperemoveTest, requires_reflink


class HistoryTest(DuperemoveTest):
    def _seed(self, name="tree", n=3):
        d = self.path(name)
        os.makedirs(d)
        for i in range(n):
            self.write(f"{name}/f{i}", os.urandom(65536))
        return d

    def test_a_row_is_recorded_per_run(self):
        d = self._seed()
        self.dm("-r", d)
        self.assertEqual(self.hf_count("run_history"), 1)
        self.dm("-r", d)
        self.assertEqual(self.hf_count("run_history"), 2)

    def test_history_view_reports_runs_and_total(self):
        d = self._seed()
        self.dm("-r", d)
        self.dm("-r", d)

        self.dm("--history")
        self.assertDmOk()
        self.assertIn("oans history", self.out)
        self.assertIn("2 runs", self.out)
        self.assertIn("reclaimed", self.out)

    def test_history_empty_before_any_run(self):
        # A hashfile that exists but has no recorded runs yet.
        d = self._seed()
        self.dm("-r", d)
        # Wipe the history rows to simulate a pre-feature / empty file.
        import sqlite3
        con = sqlite3.connect(self.hf)
        con.execute("delete from run_history")
        con.commit()
        con.close()

        self.dm("--history")
        self.assertIn("no runs recorded", self.out)

    def test_json_is_valid_and_has_expected_fields(self):
        d = self._seed(n=4)
        self.dm("-r", d)

        self.dm("--json")
        self.assertDmOk()
        obj = json.loads(self.out)
        for key in ("hashfile", "format", "files_tracked", "files_hashed",
                    "logical_bytes", "dup_groups", "reclaimable_logical_bytes",
                    "runs", "reclaimed_total_bytes", "last_run_ts"):
            self.assertIn(key, obj)
        self.assertEqual(obj["files_tracked"], 4)
        self.assertEqual(obj["runs"], 1)
        self.assertGreater(obj["last_run_ts"], 0)

    def test_reports_require_hashfile(self):
        for flag in ("--history", "--json"):
            self.dm(flag, hashfile=False)
            self.assertNotEqual(self.rc, 0, flag)
            self.assertIn("hashfile", self.out)


@requires_reflink
class HistoryReclaimTest(DuperemoveTest):
    def test_lifetime_reclaimed_accumulates_across_runs(self):
        MiB = 1 << 20
        a, b = self.mkdup("a", "b", MiB)
        self.dm("-rd", self.path("."))          # reclaims one 1 MiB copy
        first = json.loads(self.dm("--json"))["reclaimed_total_bytes"]
        # Honest accounting: deduping a pair frees exactly ONE copy, not two.
        # (The fiemap "net change in shared extents" is 2 MiB; that is not it.)
        self.assertEqual(MiB, first, "one reclaimed copy for a 1 MiB pair")

        c, e = self.mkdup("c", "e", MiB)         # a second reclaimable pair
        self.dm("-rd", self.path("."))
        second = json.loads(self.dm("--json"))["reclaimed_total_bytes"]
        self.assertEqual(2 * MiB, second, "two lifetime pairs = two copies freed")
