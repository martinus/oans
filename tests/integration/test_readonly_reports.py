"""Report modes (--stats/--history/--json/-L) open the hashfile read-only.

They must (a) never write to it - so they are safe to run while another oans is
deduping the same hashfile and can't corrupt it - and (b) never recreate a file
that isn't a valid oans hashfile.
"""

import os
import sqlite3

from harness import DuperemoveTest


class ReadonlyReportsTest(DuperemoveTest):
    def _seed_hashfile(self):
        d = self.path("tree")
        os.makedirs(d)
        for i in range(3):
            self.write(f"tree/f{i}", os.urandom(4096))
        self.dm("-r", d)          # populate the hashfile (scan only, no reflink needed)

    def test_stats_works_while_hashfile_write_locked(self):
        # Deterministically hold the WAL write lock, then run a report: the old
        # code did a config-sync write on open and failed with "database is
        # locked"; a read-only open must succeed regardless.
        self._seed_hashfile()
        con = sqlite3.connect(self.hf, timeout=1)
        con.execute("BEGIN IMMEDIATE")          # acquire the write lock
        con.execute(
            "INSERT OR REPLACE INTO config VALUES ('probe', 1)")
        try:
            for flag in ("--stats", "--history", "--json", "-L"):
                self.dm(flag)
                self.assertEqual(self.rc, 0,
                                 f"{flag} failed under a held write lock:\n{self.out}")
                self.assertNotIn("locked", self.out)
        finally:
            con.rollback()
            con.close()

    def test_report_does_not_recreate_a_foreign_file(self):
        # A non-oans file must be refused, not silently clobbered/recreated.
        with open(self.hf, "w") as f:
            f.write("not an oans hashfile")
        self.dm("--stats")
        self.assertNotEqual(self.rc, 0)
        with open(self.hf) as f:
            self.assertEqual(f.read(), "not an oans hashfile")

    def test_stats_does_not_modify_the_hashfile(self):
        self._seed_hashfile()
        before = os.stat(self.hf).st_mtime_ns
        self.dm("--stats")
        self.assertEqual(self.rc, 0)
        # A read-only report leaves the main db file untouched.
        self.assertEqual(os.stat(self.hf).st_mtime_ns, before)
