#!/usr/bin/env python3
"""Hermetic tests for lint-escape.py. No compiler, no build.

A lint that cannot fail reads exactly like a clean tree. This one had that hole
for its whole life (#276): its lookahead wanted a `,` or `)` after the path, and
the argument text it searched had the call's closing `)` already cut off, so a
path passed as the *last* argument - the most common place for one - was never
checked. Five raw prints of scanned names sat in src/ behind it. Every case
below is a shape src/ uses.
"""

import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
_spec = importlib.util.spec_from_file_location("lint_escape",
                                               HERE / "lint-escape.py")
lint = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(lint)


def hits(body: str) -> list[str]:
    """What the lint says about one C file holding `body`."""
    with tempfile.TemporaryDirectory() as d:
        p = Path(d) / "t.c"
        p.write_text(body)
        return [why for _line, why, _text in lint.check_file(p)]


class LintEscapeTest(unittest.TestCase):
    def test_a_path_as_the_last_argument_is_flagged(self):
        self.assertEqual(["path printed unescaped"],
                         hits('eprintf("%s\\n", path);\n'))

    def test_a_path_as_the_last_argument_of_a_wrapped_call_is_flagged(self):
        self.assertEqual(["extent->e_file->filename printed unescaped"],
                         hits('vprintf("%s: Skipping dedupe.\\n",\n'
                              '\textent->e_file->filename);\n'))

    def test_a_path_before_another_argument_is_flagged(self):
        self.assertEqual(["path printed unescaped"],
                         hits('eprintf("%s x %d\\n", path, 1);\n'))

    def test_an_escaped_copy_is_fine(self):
        self.assertEqual([], hits('declare_display_path(disp, path);\n'
                                  'eprintf("%s\\n", disp);\n'))

    def test_a_name_that_only_ends_in_path_is_not_a_path(self):
        self.assertEqual([], hits('eprintf("%s\\n", hashpath);\n'))

    def test_a_path_inside_the_format_string_is_not_an_argument(self):
        self.assertEqual([], hits('eprintf("path %s\\n", disp);\n'))

    def test_a_waiver_holds(self):
        self.assertEqual([], hits('/* escape-ok: our own argument. */\n'
                                  'eprintf("%s\\n", filename);\n'))


if __name__ == "__main__":
    # Quiet on success, like report-selftest, and a real exit status.
    import io

    buf = io.StringIO()
    tests = unittest.TestLoader().loadTestsFromModule(sys.modules[__name__])
    result = unittest.TextTestRunner(verbosity=2, stream=buf).run(tests)
    if not result.wasSuccessful():
        sys.stderr.write(buf.getvalue())
        sys.exit(1)
    print(f"lint-escape-selftest: {result.testsRun} tests, "
          "last-argument paths included")
