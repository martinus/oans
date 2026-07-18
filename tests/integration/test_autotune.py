"""--autotune: empirical io-threads calibration.

These assertions don't depend on reflink or on which thread count wins (that is
hardware-specific); they check the mechanism: a sample is measured, a value is
recommended and persisted into the hashfile config, and a later ordinary run
picks that value up automatically. Kept fast with one round and a tiny sample.
"""

import os
import re
import subprocess
import unittest

from harness import DuperemoveTest, DUPEREMOVE


class AutotuneTest(DuperemoveTest):
    # One round over a handful of files keeps the whole test well under a second.
    ENV = {"DUPEREMOVE_AUTOTUNE_ROUNDS": "1"}

    def _run(self, *args, env=None):
        run_env = dict(os.environ, **(env or {}))
        proc = subprocess.run([DUPEREMOVE, *args], stdout=subprocess.PIPE,
                              stderr=subprocess.STDOUT, text=True, env=run_env)
        return proc.returncode, proc.stdout

    def _make_tree(self, n=6):
        for i in range(n):
            self.mkrand(f"tree/f{i}.bin", 64 * 1024)
        return self.path("tree")

    def test_recommends_and_persists(self):
        tree = self._make_tree()
        rc, out = self._run("--autotune", "-r", "--hashfile", self.hf, tree,
                            env=self.ENV)
        self.assertEqual(rc, 0, out)

        m = re.search(r"Recommended: --io-threads=(\d+)", out)
        self.assertIsNotNone(m, f"no recommendation in output:\n{out}")
        rec = int(m.group(1))
        self.assertGreaterEqual(rec, 1)

        stored = self.hf_scalar(
            "select keyval from config where keyname='autotune_io_threads'")
        self.assertEqual(stored, rec, "recommendation stored in hashfile config")

    def test_stored_value_used_by_later_run(self):
        tree = self._make_tree()
        self._run("--autotune", "-r", "--hashfile", self.hf, tree, env=self.ENV)
        stored = self.hf_scalar(
            "select keyval from config where keyname='autotune_io_threads'")
        self.assertIsNotNone(stored)

        # A plain verbose run (no explicit --io-threads) must report using it.
        rc, out = self._run("-rv", "--hashfile", self.hf, tree)
        self.assertEqual(rc, 0, out)
        self.assertIn(f"autotuned --io-threads={stored}", out,
                      f"stored value not picked up:\n{out}")

    def test_explicit_io_threads_overrides_stored(self):
        tree = self._make_tree()
        self._run("--autotune", "-r", "--hashfile", self.hf, tree, env=self.ENV)
        # With an explicit count, the stored value must be ignored silently.
        rc, out = self._run("-rv", "--io-threads=2", "--hashfile", self.hf, tree)
        self.assertEqual(rc, 0, out)
        self.assertNotIn("autotuned", out)

    def test_requires_a_path(self):
        rc, out = self._run("--autotune", "--hashfile", self.hf)
        self.assertNotEqual(rc, 0)
        self.assertIn("needs a file or directory", out)


if __name__ == "__main__":
    unittest.main()
