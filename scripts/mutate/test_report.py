#!/usr/bin/env python3
"""Hermetic tests for report.py. No compiler, no build, no mutation run.

report.py exists to stop a survivor count being misread, and its first version
misread one itself: the walk back over a signature ran up into the doc comment
above it, so every one of read_rotational's 36 mutants was filed under the word
`parent`, taken from prose describing a partition's parent directory. Nothing
about the output looked wrong - a plausible name with a plausible count.

That is the whole hazard of this tool. It is only ever read as an explanation
of a number, so a wrong grouping does not fail, it persuades. These tests are
the parser's, and every case below is a shape that appears in src/.
"""

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import report  # noqa: E402

SRC = '''\
#include <stdio.h>

static int plain(int a)
{
	return a;
}

/*
 * A doc comment whose prose mentions a call (like this one) - which is what
 * broke the first parser, since `(` in a comment reads as a signature.
 */
static int commented(int a)
{
	return a + 1;
}

unsigned int wrapped_signature(const struct thing *p,
			       unsigned int n)
{
	return n;
}

static void *returns_a_pointer(void)
{
	return NULL;
}
'''


class TestFunctionMap(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.src = Path(self.dir.name) / "s.c"
        self.src.write_text(SRC)
        self.names = report.function_map(self.src)

    def tearDown(self):
        self.dir.cleanup()

    def at(self, needle):
        """The name reported for the line containing `needle`."""
        for i, line in enumerate(SRC.split("\n"), 1):
            if needle in line:
                return self.names[i]
        self.fail(f"{needle!r} is not in the fixture")

    def test_a_body_line_is_attributed_to_its_function(self):
        self.assertEqual("plain", self.at("return a;"))

    def test_a_doc_comment_does_not_become_the_name(self):
        # The regression this file is named for. `parent` came from prose.
        self.assertEqual("commented", self.at("return a + 1;"))

    def test_a_signature_wrapped_across_lines_keeps_its_name(self):
        # The continuation line looks nothing like a definition, which is why
        # the parser keys off `{` in column 0 rather than matching the decl.
        self.assertEqual("wrapped_signature", self.at("return n;"))

    def test_a_pointer_return_type_is_not_part_of_the_name(self):
        self.assertEqual("returns_a_pointer", self.at("return NULL;"))

    def test_code_outside_any_function_is_file_scope(self):
        self.assertEqual("<file scope>", self.at("#include"))


class TestPopulation(unittest.TestCase):
    """The guard on the per-mutant diff.

    Two runs of a *changed* source generate different mutants, and diffing them
    per line attributes survivors to whatever now sits at that number. Doing
    exactly that by hand once produced a confident report of a regression in a
    function the change had not touched.
    """

    def test_the_same_mutants_compare_equal_in_any_order(self):
        a = [{"line": 2, "name": "x"}, {"line": 1, "name": "y"}]
        b = [{"line": 1, "name": "y"}, {"line": 2, "name": "x"}]
        self.assertEqual(report.population(a), report.population(b))

    def test_a_moved_line_is_a_different_population(self):
        a = [{"line": 10, "name": "< -> <="}]
        b = [{"line": 11, "name": "< -> <="}]
        self.assertNotEqual(report.population(a), report.population(b))

    def test_a_different_mutation_on_the_same_line_differs(self):
        a = [{"line": 10, "name": "< -> <="}]
        b = [{"line": 10, "name": "< -> >"}]
        self.assertNotEqual(report.population(a), report.population(b))


class TestGrouped(unittest.TestCase):
    def test_totals_and_survivors_are_counted_per_function(self):
        names = ["<file scope>", "f", "f", "g"]
        res = [{"line": 1, "verdict": "survived"},
               {"line": 2, "verdict": "caught"},
               {"line": 3, "verdict": "survived"}]
        self.assertEqual({"f": [1, 2], "g": [1, 1]}, report.grouped(res, names))

    def test_a_line_outside_the_map_is_not_silently_dropped(self):
        # Better a visible bucket than a total that quietly stops adding up.
        g = report.grouped([{"line": 999, "verdict": "survived"}], ["a", "b"])
        self.assertEqual({"<unknown line>": [1, 1]}, g)


class TestEndToEnd(unittest.TestCase):
    """Drive the script the way a person does, since the reports are its API."""

    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        d = Path(self.dir.name)
        self.src = d / "s.c"
        self.src.write_text(SRC)
        line = next(i for i, l in enumerate(SRC.split("\n"), 1)
                    if "return a + 1;" in l)
        self.before = self.write(d / "b.json", [
            {"line": line, "name": "+ -> -", "verdict": "survived"},
            {"line": line, "name": "1 -> 2", "verdict": "survived"}])
        self.after = self.write(d / "a.json", [
            {"line": line, "name": "+ -> -", "verdict": "caught"},
            {"line": line, "name": "1 -> 2", "verdict": "survived"}])

    def tearDown(self):
        self.dir.cleanup()

    def write(self, path, results):
        counts = {v: sum(1 for r in results if r["verdict"] == v)
                  for v in ("caught", "compiler", "hang", "oom", "survived")}
        path.write_text(json.dumps({
            "environment": {"file": str(self.src)},
            "counts": counts, "results": results}))
        return path

    def run_it(self, *args):
        p = subprocess.run([sys.executable, str(HERE / "report.py"), *args],
                           capture_output=True, text=True)
        self.assertEqual(0, p.returncode, p.stderr)
        return p.stdout

    def test_a_single_run_names_the_function_not_the_line(self):
        out = self.run_it(str(self.before))
        self.assertIn("commented", out)
        self.assertIn("2 survivors of 2", out)

    def test_a_diff_names_what_was_killed(self):
        out = self.run_it(str(self.before), str(self.after))
        self.assertIn("killed by the change", out)
        self.assertIn("+ -> -", out)
        # And says nothing about the one that did not move.
        self.assertNotIn("NEWLY SURVIVING", out)

    def test_a_survivor_that_appears_is_called_out(self):
        out = self.run_it(str(self.after), str(self.before))
        self.assertIn("NEWLY SURVIVING", out)

    def test_a_changed_source_refuses_the_per_mutant_diff(self):
        moved = self.write(Path(self.dir.name) / "m.json", [
            {"line": 99, "name": "+ -> -", "verdict": "survived"}])
        out = self.run_it(str(self.before), str(moved))
        self.assertIn("different mutants", out)
        self.assertNotIn("killed by the change", out)

    def test_two_different_files_are_refused_outright(self):
        other = Path(self.dir.name) / "o.json"
        other.write_text(json.dumps({
            "environment": {"file": "src/elsewhere.c"},
            "counts": {"survived": 0, "caught": 0, "compiler": 0,
                       "hang": 0, "oom": 0},
            "results": []}))
        p = subprocess.run(
            [sys.executable, str(HERE / "report.py"), str(self.before), str(other)],
            capture_output=True, text=True)
        self.assertNotEqual(0, p.returncode)
        self.assertIn("different files swept", p.stderr)


if __name__ == "__main__":
    # One line on success, like the lint-*.py scripts beside it, and a real
    # exit status. Deliberately not `unittest.main() | tail -1` from the
    # makefile: sh reports a pipeline's *last* command, so the tail would
    # return 0 however the tests went and the check could never fail - which
    # is the exact shape of thing the rest of this directory exists to catch.
    import io

    buf = io.StringIO()
    loader = unittest.TestLoader().loadTestsFromModule(sys.modules[__name__])
    result = unittest.TextTestRunner(verbosity=2, stream=buf).run(loader)
    if not result.wasSuccessful():
        sys.stderr.write(buf.getvalue())
        sys.exit(1)
    print(f"report-selftest: {result.testsRun} tests, parser and diff guards")
