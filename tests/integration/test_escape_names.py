"""File names are untrusted input and must never reach the terminal raw (#202).

Anyone who can create a file inside a scanned tree picks bytes that oans then
prints to the administrator's terminal. A `\\r` overwrites the line above it (the
summary, or the progress block); an ESC sequence can clear the screen, change
colours, or - on some terminals - wedge them. So every path oans prints is
escaped: reports, error messages, the -L listing and --stats.

These names are made with bytes paths on purpose; btrfs and xfs accept any byte
but NUL and '/'.
"""

import json
import os
import subprocess
import unittest

from harness import DUPEREMOVE, DuperemoveTest, requires_reflink

# One name per trick: a carriage return that would rewrite the previous line, a
# screen-clearing CSI, a BEL, and a DEL.
EVIL_NAMES = [
    b"cr\rSUCCESS-reclaimed-9-TiB.bin",
    b"esc\x1b[2J\x1b[31mred.bin",
    b"bel\x07.bin",
    b"del\x7f.bin",
]


class EscapeNamesTest(DuperemoveTest):
    def run_raw(self, *args, expect_ok=True):
        """Run oans with the output captured as raw bytes, not decoded text."""
        p = subprocess.run(
            [DUPEREMOVE, "--io-threads=4", "--hashfile", self.hf, *args],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        if expect_ok:
            self.assertEqual(p.returncode, 0, p.stderr.decode(errors="replace"))
        return p.stdout + p.stderr

    def assertNoControlBytes(self, out):
        """No byte a terminal acts on, other than the newlines oans writes.

        Output is not a tty here, so oans emits no ANSI of its own - anything
        left is from a file name.
        """
        for b in (b"\x1b", b"\r", b"\x07", b"\x7f"):
            self.assertNotIn(b, out,
                             f"raw {b!r} reached the terminal:\n"
                             f"{out.decode(errors='replace')}")

    def _tree(self, size=200_000):
        """A tree of duplicate pairs, every one of them evilly named."""
        d = self.path("tree")
        os.makedirs(d, exist_ok=True)
        for i, name in enumerate(EVIL_NAMES):
            data = os.urandom(size)
            for side in (b"a-", b"b-"):
                with open(os.path.join(d.encode(), side + name), "wb") as f:
                    f.write(data)
        return d

    @requires_reflink
    def test_dedupe_report_and_summary_escape_names(self):
        out = self.run_raw("-rdv", self._tree())
        self.assertNoControlBytes(out)
        # The names are still identifiable, just spelled safely.
        self.assertIn(b"cr\\rSUCCESS", out)
        self.assertIn(b"esc\\x1b[2J", out)
        self.assertIn(b"bel\\x07", out)
        self.assertIn(b"del\\x7f", out)

    def test_scan_and_listing_escape_names(self):
        d = self._tree(size=8192)
        self.assertNoControlBytes(self.run_raw("-rv", d))
        self.assertNoControlBytes(self.run_raw("-L"))

    def test_error_messages_escape_names(self):
        """The message about an unreadable file names it - safely."""
        if os.geteuid() == 0:
            self.skipTest("running as root defeats chmod 000")
        d = self.path("tree")
        os.makedirs(d, exist_ok=True)
        victim = os.path.join(d.encode(), b"esc\x1b[2Jlocked.bin")
        with open(victim, "wb") as f:
            f.write(os.urandom(65536))
        os.chmod(victim, 0)

        out = self.run_raw("-rv", d)
        self.assertIn(b"esc\\x1b[2Jlocked.bin", out)
        self.assertNoControlBytes(out)

    def test_stats_escapes_stored_paths(self):
        d = self._tree(size=8192)
        self.run_raw("-r", d)
        self.assertNoControlBytes(self.run_raw("--stats"))

    def test_json_report_escapes_and_stays_parseable(self):
        d = self._tree(size=8192)
        self.run_raw("-r", d)
        out = self.run_raw("--json")
        self.assertNoControlBytes(out)
        json.loads(out.decode())          # still one valid object

    def test_a_plain_name_is_printed_unchanged(self):
        """The escape must be invisible for the names people actually have.

        A duplicate pair, because a name only reaches the report when its file
        is in a group.
        """
        self.mkdup("tree/café-Β a.bin", "tree/café-Β b.bin", 200_000)
        out = self.run_raw("-rv", self.path("tree"))
        self.assertIn("café-Β a.bin".encode(), out)
        self.assertNotIn(b"\\x", out)


if __name__ == "__main__":
    unittest.main()
