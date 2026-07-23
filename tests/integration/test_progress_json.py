"""Machine-readable progress stream: --progress=json.

With --progress=json, oans suppresses the ANSI UI and streams newline-delimited
JSON progress events to *stderr* (one per phase / ~1s), ending with a
{"event":"done", ...} line, while stdout keeps its normal (human / compat)
output. This lets scheduled and non-interactive runs be monitored.
"""

import json
import os
import subprocess

from harness import DuperemoveTest, requires_reflink, DUPEREMOVE


class ProgressJsonTest(DuperemoveTest):
    def _run(self, *args):
        """Run oans with --progress=json, stdout/stderr captured separately."""
        cmd = [DUPEREMOVE, "-q", "--io-threads=4", "--hashfile", self.hf,
               "--progress=json", *args]
        p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                           text=True)
        # Every non-blank stderr line must be a JSON object.
        events = [json.loads(ln) for ln in p.stderr.splitlines() if ln.strip()]
        return p, events

    def _dupe_tree(self, n=8, size=200_000):
        for i in range(n):
            self.mkdup(f"tree/a{i}", f"tree/b{i}", size)
        return os.path.join(self.work, "tree")

    @requires_reflink
    def test_dedupe_run_streams_json_and_done(self):
        n = 8
        d = self._dupe_tree(n)
        p, events = self._run("-rd", d)
        self.assertEqual(p.returncode, 0)

        phases = {e["phase"] for e in events if "phase" in e}
        done = [e for e in events if e.get("event") == "done"]

        # A scan-side phase and the dedupe phase are always reported (each
        # phase's thread emits at least one event); fast runs may coalesce
        # scanning/hashing, so accept either.
        self.assertTrue(phases & {"scanning", "hashing"}, phases)
        self.assertIn("dedupe", phases)

        self.assertEqual(len(done), 1)
        d0 = done[0]
        self.assertEqual(d0["files_scanned"], 2 * n)
        self.assertEqual(d0["groups_deduped"], n)
        self.assertGreater(d0["reclaimed_bytes"], 0)
        self.assertIn("elapsed_sec", d0)

        # stdout stays free of the JSON stream (it is on stderr).
        self.assertNotIn("{", p.stdout)

    def test_scan_only_reports_zero_groups(self):
        # No -d: nothing deduped, but the stream and done event still appear.
        for i in range(3):
            self.write(f"tree/f{i}", os.urandom(65536))
        d = os.path.join(self.work, "tree")
        p, events = self._run("-r", d)
        self.assertEqual(p.returncode, 0)

        done = [e for e in events if e.get("event") == "done"]
        self.assertEqual(len(done), 1)
        self.assertEqual(done[0]["groups_deduped"], 0)
        self.assertEqual(done[0]["files_scanned"], 3)

    def test_hashing_event_fields(self):
        # If a hashing event is present, it carries the expected numeric keys.
        for i in range(4):
            self.write(f"tree/f{i}", os.urandom(65536))
        d = os.path.join(self.work, "tree")
        _p, events = self._run("-r", d)
        for e in events:
            if e.get("phase") == "hashing":
                for k in ("files", "files_total", "bytes", "bytes_total"):
                    self.assertIn(k, e)

    def test_invalid_progress_value_is_rejected(self):
        p = subprocess.run(
            [DUPEREMOVE, "-r", "--progress=xml", self.work],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        self.assertNotEqual(p.returncode, 0)
        self.assertIn("--progress", p.stderr)
