"""The hashfile is branded with a SQLite application_id ("oans") so oans owns
its own format: it stamps its own files, rebuilds an unbranded duperemove or
pre-brand oans hashfile, and leaves every other database alone (#275) - a
mistyped --hashfile can name another program's data.
"""

import sqlite3
import subprocess
from harness import DuperemoveTest, DUPEREMOVE

OANS_APP_ID = 0x6F616E73  # ascii "oans"


def app_id(path):
    # Note: sqlite3's context manager commits but does NOT close, and a lingering
    # connection holds a WAL lock that breaks oans's unlink+recreate. Close it.
    con = sqlite3.connect(path)
    try:
        return con.execute("PRAGMA application_id").fetchone()[0]
    finally:
        con.close()


def set_app_id(path, value):
    con = sqlite3.connect(path)
    try:
        con.execute(f"PRAGMA application_id = {value}")
        con.commit()
    finally:
        con.close()


def schema(path):
    """Every schema object, with its SQL, in a stable order."""
    con = sqlite3.connect(path)
    try:
        return con.execute("select type, name, sql from sqlite_master "
                           "order by name").fetchall()
    finally:
        con.close()


def journal_mode(path):
    con = sqlite3.connect(path)
    try:
        return con.execute("PRAGMA journal_mode").fetchone()[0]
    finally:
        con.close()


class AppIdTest(DuperemoveTest):
    def test_fresh_hashfile_is_branded(self):
        self.mkrand("tree/a", 8000)
        self.scan(self.path("tree"))
        self.assertDmOk()
        self.assertEqual(OANS_APP_ID, app_id(self.hf), "fresh hashfile carries the oans brand")

    def test_unbranded_is_refused_and_rebuilt(self):
        # A file without the brand (application_id 0: a pre-brand oans file or a
        # duperemove one) is strictly refused and recreated fresh.
        self.mkrand("tree/a", 8000)
        self.mkrand("tree/b", 8000)
        self.scan(self.path("tree"))
        set_app_id(self.hf, 0)
        self.scan(self.path("tree"))
        self.assertDmOk()
        self.assertIn("Recreating", self.out, "unbranded hashfile rebuilt")
        self.assertEqual(OANS_APP_ID, app_id(self.hf), "recreated as an oans file")

    def test_foreign_application_is_left_alone(self):
        # Branded by some other program: refused, and not touched. It used to
        # be unlinked and recreated.
        self.mkrand("tree/a", 8000)
        self.scan(self.path("tree"))
        set_app_id(self.hf, 0x12345678)
        before = schema(self.hf)
        self.scan(self.path("tree"))
        self.assertEqual(1, self.rc, self.out)
        self.assertIn("belongs to another program", self.out)
        self.assertEqual(0x12345678, app_id(self.hf))
        self.assertEqual(before, schema(self.hf))

    def foreign_db(self, sql, appid):
        con = sqlite3.connect(self.hf)
        try:
            con.executescript(sql)
            con.execute(f"PRAGMA application_id = {appid}")
            con.commit()
        finally:
            con.close()
        return schema(self.hf)

    def test_another_programs_database_is_left_alone(self):
        before = self.foreign_db(
            "create table notes(x); insert into notes values('keep me');",
            0x12345678)
        self.mkrand("tree/a", 8000)
        self.scan(self.path("tree"))
        self.assertEqual(1, self.rc, self.out)
        self.assertEqual(before, schema(self.hf), "no oans tables added")
        self.assertEqual(0x12345678, app_id(self.hf), "brand kept")

        # A report writes nothing either - the journal mode is stored in the
        # file, and it was set before the brand was ever looked at.
        self.dm("--stats")
        self.assertNotEqual(0, self.rc)
        self.assertEqual("delete", journal_mode(self.hf))
        self.assertEqual(before, schema(self.hf))

    def test_an_unbranded_database_with_a_config_table_is_not_unlinked(self):
        # No brand, and a config table without oans's rows: the version check
        # used to fail on it, and the recreate path deleted the file.
        before = self.foreign_db(
            "create table files(a); create table config(keyname, keyval);"
            "insert into config values('owner', 'me');", 0)
        self.mkrand("tree/a", 8000)
        self.scan(self.path("tree"))
        self.assertEqual(1, self.rc, self.out)
        self.assertIn("is not an oans hashfile", self.out)
        self.assertEqual(before, schema(self.hf), "the file is intact")

    def test_rebuild_is_compacted(self):
        # A from-scratch build is written at insert density, so oans VACUUMs it
        # once at the end. Run non-quiet (the message is suppressed by -q) after
        # forcing a rebuild and check the compaction fired.
        self.mkrand("tree/a", 8000)
        self.scan(self.path("tree"))
        set_app_id(self.hf, 0)  # an unbranded hashfile: rebuilt on the next run
        proc = subprocess.run(
            [DUPEREMOVE, "-r", "--hashfile", self.hf, self.path("tree")],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        self.assertIn("Recreating", proc.stdout)
        self.assertIn("Compacting", proc.stdout, "rebuilt hashfile is VACUUMed")
        self.assertEqual(0, sqlite3.connect(self.hf).execute(
            "PRAGMA freelist_count").fetchone()[0], "no free pages after compaction")
