"""Command-line edge cases that exited quietly or wrote what nobody asked for.

Each of these came out of a source audit: a mode run with nothing to act on
that said nothing and exited 0, a typo in the hashfile path that left an empty
hashfile behind, a value outside what the code is built for that was
accepted, and --json output that a JSON parser rejects.
"""

import json
import os
import subprocess

from harness import DUPEREMOVE, DuperemoveTest


class CliEdgeCaseTest(DuperemoveTest):
    def _run(self, *args, cwd=None, stdin=None):
        return subprocess.run([DUPEREMOVE, *args], cwd=cwd, input=stdin,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                              text=True)

    def _tree(self):
        self.mkrand("tree/a.bin", 50000)
        self.sync()
        return self.path("tree")

    def test_remove_with_no_paths_is_an_error(self):
        """-R with nothing to remove did nothing and exited 0."""
        self.dm("-r", self._tree())
        self.assertDmOk()
        self.dm("-R")
        self.assertEqual(1, self.rc, self.out)
        self.assertIn("-R takes the paths", self.out)

    def test_maintenance_on_a_missing_hashfile_creates_nothing(self):
        """Opening a hashfile creates it, so a typo left an empty one behind:
        the same trap #284 closed for the bare replay."""
        typo = os.path.join(self.work, "typo.db")
        for args in (("-R", self.path("x")), ("--prune-block-hashes",)):
            self.dm(*args, "--hashfile", typo, hashfile=False)
            self.assertFalse(os.path.exists(typo), f"{args} created it")
            self.assertEqual(1, self.rc, f"{args}: {self.out}")
            self.assertIn("there is no hashfile", self.out)

    def test_the_block_size_must_be_a_power_of_two(self):
        tree = self._tree()
        for bs in ("5000", "96K", "1000K"):
            self.dm("-r", "-b", bs, tree)
            self.assertEqual(1, self.rc, f"-b {bs}: {self.out}")
            self.assertIn("power of two", self.out)
        for bs in ("4K", "64K", "1M"):
            self.dm("-r", "-b", bs, tree)
            self.assertDmOk(f"-b {bs}")

    def test_question_mark_is_a_usage_error_that_says_so(self):
        """-? was in the getopt string but fell into the error branch, so it
        exited 1 having printed nothing at all."""
        proc = self._run("-?")
        self.assertEqual(1, proc.returncode)
        self.assertIn("--help", proc.stderr)
        self.assertEqual("", proc.stdout)

    def test_a_file_list_on_stdin_does_not_probe_a_path_named_dash(self):
        """With '-', the first root is the string "-": storage detection
        probed whatever has that name in the current directory."""
        f = self.mkrand("tree/a.bin", 50000)
        self.sync()
        os.mkdir(self.path("-"))     # a real directory, on real storage
        proc = self._run("-v", "--hashfile", self.hf, "-", cwd=self.work,
                         stdin=f + "\n")
        self.assertEqual(0, proc.returncode, proc.stdout + proc.stderr)
        storage = [ln for ln in proc.stdout.splitlines()
                   if ln.startswith("Storage:")]
        self.assertEqual(1, len(storage), proc.stdout)
        self.assertIn("unknown media", storage[0])

    def test_json_stays_valid_for_a_name_that_is_not_utf8(self):
        """A Latin-1 byte went out raw, and json.loads() refused the whole
        object. It now reads as the code point of that byte."""
        hf = os.path.join(os.fsencode(self.work), b"caf\xe9.db")
        self.dm("-r", self._tree(), "--hashfile", os.fsdecode(hf),
                hashfile=False)
        self.assertDmOk()
        out = subprocess.run([DUPEREMOVE, "--json", "--hashfile",
                              os.fsdecode(hf)],
                             stdout=subprocess.PIPE, check=True).stdout
        metrics = json.loads(out.decode("utf-8"))
        self.assertEqual(self.work + "/café.db", metrics["hashfile"])

    def test_json_names_a_missing_scan_config_as_null(self):
        """A run fed from stdin stores no configuration, and the key was then
        left out - a consumer reading it got a KeyError, not an answer."""
        f = self.mkrand("tree/a.bin", 50000)
        self.sync()
        self.dm("-", stdin=f + "\n")
        self.assertDmOk()
        metrics = json.loads(self.dm("--json"))
        self.assertIn("scan_configured_dedupe", metrics)
        self.assertIsNone(metrics["scan_configured_dedupe"])

    def test_report_labels_say_what_they_count(self):
        """--history's lifetime figure is files hashed, not files seen, and
        --stats' unread files include a hash a resume left unfinished."""
        tree = self._tree()
        self.dm("-r", tree)
        self.dm("-r", tree)          # the second run hashes nothing new
        out = self.dm("--history", quiet=False)
        self.assertIn("files hashed", out)
        self.assertNotIn("files seen", out)

        # What an interrupted hash of a large file leaves (#159): a row
        # with no digest yet.
        self.hf_exec("update files set digest = null")
        out = self.dm("--stats", quiet=False)
        self.assertIn("no file digest", out)
