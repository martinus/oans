"""CLI help and usage output (oans issue #86).

Running oans with no arguments, or with -h/--help, must print usage/help
*itself* and never shell out to man(1). Before the fix an uninstalled build
printed "No manual entry for oans in section 8"; now it prints a self-contained
usage summary that points at `man 8 oans` for the full reference.
"""

import subprocess
import unittest

from harness import DUPEREMOVE, DuperemoveTest


class HelpTest(DuperemoveTest):
    """These only exercise argument parsing, so no scratch fs is needed."""

    def _run(self, *args):
        return subprocess.run([DUPEREMOVE, *args], stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE, text=True)

    def test_help_flag_prints_help_and_exits_zero(self):
        for flag in ("-h", "--help"):
            proc = self._run(flag)
            self.assertEqual(proc.returncode, 0, f"{flag} exits 0")
            self.assertIn("Usage: oans", proc.stdout, f"{flag} prints usage")
            self.assertIn("--hashfile", proc.stdout, f"{flag} lists options")
            self.assertNotIn("No manual entry", proc.stdout + proc.stderr)

    def test_no_arguments_prints_usage_to_stderr_and_fails(self):
        proc = self._run()
        self.assertNotEqual(proc.returncode, 0, "no-args is a usage error")
        self.assertIn("Usage: oans", proc.stderr, "usage goes to stderr")
        self.assertNotIn("No manual entry", proc.stdout + proc.stderr)

    def test_help_is_self_contained_not_man(self):
        # The whole point of #86: help must not exec man(1). A self-contained
        # listing (mentioning man only as a pointer) proves man wasn't run.
        proc = self._run("--help")
        self.assertIn("Reporting and maintenance", proc.stdout)
        self.assertIn("man 8 oans", proc.stdout)


if __name__ == "__main__":
    unittest.main()
