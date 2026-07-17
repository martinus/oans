"""The hashfile is branded with a SQLite application_id ("oans") so oans owns
its own format: it stamps its own files and strictly refuses any hashfile that
does not carry the brand (a foreign program's, or a pre-brand/duperemove one),
recreating it fresh.
"""

import sqlite3
from harness import DuperemoveTest

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

    def test_foreign_application_is_refused(self):
        # A hashfile branded by some other program is refused and recreated.
        self.mkrand("tree/a", 8000)
        self.mkrand("tree/b", 8000)
        self.scan(self.path("tree"))
        set_app_id(self.hf, 0x12345678)
        self.scan(self.path("tree"))
        self.assertDmOk()
        self.assertIn("Recreating", self.out, "foreign hashfile rebuilt")
        self.assertEqual(OANS_APP_ID, app_id(self.hf), "rebuilt as an oans file")
