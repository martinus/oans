"""A failed hashfile write during the scan (#274).

The scan keeps one write transaction open for ~10 s across many files, and the
ids of the rows in it are already handed to the hashing workers. Rolling the
whole batch back for one failed file used to leave those workers writing
against ids that were gone or reused: thousands of constraint failures, most
of the tree missing, sometimes one file's digest on another file's row - and
exit 0.

DUPEREMOVE_WRITE_FAIL_AT=N fails the final write of the Nth file to finish
hashing. DUPEREMOVE_WRITE_FAIL_LOSES_BATCH makes that failure take the whole
transaction with it, as SQLite may do by itself on a full disk.
"""

import os
import sqlite3
from harness import DuperemoveTest

NFILES = 400
FAIL_AT = "50"


class WriteFailureTest(DuperemoveTest):
    def setUp(self):
        super().setUp()
        for i in range(NFILES):
            self.mkrand(f"tree/d{i % 10}/f{i}", 16384 + i)
        self.clean = os.path.join(self.work, "clean.db")
        self.dm("-r", self.path("tree"), "--hashfile", self.clean,
                hashfile=False)
        self.assertEqual(0, self.rc, self.out)

    def counts(self):
        """(rows, digests, digests the clean run has for that name, extent
        rows, extent rows whose digest the clean run has at that offset)"""
        con = sqlite3.connect(self.hf)
        try:
            con.execute("attach ? as k", (self.clean,))
            return con.execute("""
                select (select count(*) from main.files),
                       (select count(*) from main.files
                            where digest is not null),
                       (select count(*) from main.files f join k.files c
                            on c.filename = f.filename and c.digest = f.digest),
                       (select count(*) from main.extents),
                       (select count(*) from main.extents e
                            join main.files f on f.id = e.fileid
                            join k.files c on c.filename = f.filename
                            join k.extents ce on ce.fileid = c.id
                                and ce.loff = e.loff
                                and ce.digest = e.digest)""").fetchone()
        finally:
            con.close()

    def test_one_failed_write_loses_only_its_own_file(self):
        self.dm("-r", self.path("tree"),
                  env={"DUPEREMOVE_WRITE_FAIL_AT": FAIL_AT})
        self.assertEqual(0, self.rc, self.out)
        rows, digests, right, extents, right_extents = self.counts()
        self.assertEqual(NFILES, rows)
        self.assertEqual(NFILES - 1, digests, "one file is missing, no more")
        self.assertEqual(digests, right, "every digest is its own file's")
        self.assertEqual(extents, right_extents,
                         "every extent row is its own file's")

    def test_a_lost_batch_stops_the_scan_and_fails_the_run(self):
        self.dm("-r", self.path("tree"),
                  env={"DUPEREMOVE_WRITE_FAIL_AT": FAIL_AT,
                       "DUPEREMOVE_WRITE_FAIL_LOSES_BATCH": "1"})
        self.assertEqual(1, self.rc, self.out)
        self.assertIn("a write to the hashfile failed", self.out)
        _rows, digests, right, extents, right_extents = self.counts()
        self.assertLess(digests, NFILES, "the lost files are missing")
        self.assertEqual(digests, right, "every digest is its own file's")
        self.assertEqual(extents, right_extents,
                         "every extent row is its own file's")

        self.scan(self.path("tree"))
        self.assertEqual(0, self.rc, self.out)
        rows, digests, right, extents, right_extents = self.counts()
        self.assertEqual((NFILES, NFILES, NFILES), (rows, digests, right),
                         "the next run hashes what was lost")
        self.assertEqual(extents, right_extents)
