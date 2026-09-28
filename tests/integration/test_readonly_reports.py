"""Report modes (--stats/--history/--json/-L) open the hashfile read-only.

They must (a) never write to it - so they are safe to run while another oans is
deduping the same hashfile and can't corrupt it - and (b) never recreate a file
that isn't a valid oans hashfile.
"""

import os
import sqlite3
import subprocess
import time

from harness import DuperemoveTest, DUPEREMOVE


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

    def test_reports_read_a_hashfile_no_scan_has_upgraded(self):
        """A read-only open does not run create_tables(), so a hashfile from
        v1.10.0 or older has neither files.nr_extents nor scan_checkpoints.
        Every report used to fail preparing statements that name them, so an
        exporter polling --json broke until the next scan (#275)."""
        self._seed_hashfile()
        con = sqlite3.connect(self.hf)
        try:
            con.executescript("alter table files drop column nr_extents;"
                              "drop table scan_checkpoints;")
        finally:
            con.close()
        for flag in ("--stats", "--history", "--json", "-L"):
            self.dm(flag)
            self.assertEqual(0, self.rc, f"{flag}:\n{self.out}")
        self.assertIn("tree/f0", self.dm("-L"))
        con = sqlite3.connect(self.hf)
        try:
            cols = [r[1] for r in con.execute("pragma table_info(files)")]
        finally:
            con.close()
        self.assertNotIn("nr_extents", cols, "and still wrote nothing")

    def test_reports_read_a_run_history_from_before_its_columns(self):
        """v1.1.0 had no run_history, and v1.4-v1.6 lacked two of its
        columns. Branded 5.0 files, so nothing refuses them - but
        --history and --json selected the missing columns."""
        self._seed_hashfile()
        con = sqlite3.connect(self.hf)
        try:
            con.executescript(
                "alter table run_history drop column skip_unsupported_fs;"
                "alter table run_history drop column readonly_subvols;")
            ncols = len(con.execute(
                "pragma table_info(run_history)").fetchall())
        finally:
            con.close()
        for flag in ("--history", "--json"):
            self.dm(flag)
            self.assertEqual(0, self.rc, f"{flag}:\n{self.out}")
        self.assertIn('"runs": 1', self.dm("--json"))

        con = sqlite3.connect(self.hf)
        try:
            self.assertEqual(ncols, len(con.execute(
                "pragma table_info(run_history)").fetchall()),
                "the table was read, not upgraded")
            con.execute("drop table run_history")
            con.commit()
        finally:
            con.close()
        for flag in ("--history", "--json"):
            self.dm(flag)
            self.assertEqual(0, self.rc, f"{flag}:\n{self.out}")

    def test_a_locked_hashfile_is_waited_for_not_refused(self):
        """Identification reads the file before anything else does. A lock
        held at that moment must be waited out; it was read as "not an oans
        hashfile"."""
        self._seed_hashfile()
        con = sqlite3.connect(self.hf, isolation_level=None)
        try:
            con.execute("pragma journal_mode = delete")
            con.execute("begin exclusive")      # readers block on this
            proc = subprocess.Popen(
                [DUPEREMOVE, "--stats", "--hashfile", self.hf],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            time.sleep(1)
            con.execute("rollback")
        finally:
            con.close()
        out, _ = proc.communicate(timeout=60)
        self.assertEqual(0, proc.returncode, out)

    def test_a_report_leaves_the_journal_mode_alone(self):
        """The journal mode is stored in the file, so setting it is a write.
        A report takes the file as it finds it (#275)."""
        self._seed_hashfile()
        con = sqlite3.connect(self.hf)
        try:
            con.execute("pragma journal_mode = delete")
        finally:
            con.close()
        self.dm("--stats")
        self.assertEqual(0, self.rc, self.out)
        con = sqlite3.connect(self.hf)
        try:
            mode = con.execute("pragma journal_mode").fetchone()[0]
        finally:
            con.close()
        self.assertEqual("delete", mode)
