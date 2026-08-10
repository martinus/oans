"""Machine-readable progress stream: --progress=json.

With --progress=json, oans suppresses the ANSI UI and streams newline-delimited
JSON progress events to *stderr* (one per phase / ~1s), ending with a
{"event":"done", ...} line, while stdout keeps its normal (human / compat)
output. This lets scheduled and non-interactive runs be monitored.
"""

import json
import os
import subprocess
import unittest

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

        # A dedupe tick carries the dedupe schema.
        dedupe_evs = [e for e in events if e.get("phase") == "dedupe"]
        self.assertTrue(dedupe_evs)
        for k in ("groups", "groups_total", "reclaimed_bytes", "elapsed_sec"):
            self.assertIn(k, dedupe_evs[0])

        # Every line has elapsed_sec and it never goes backwards.
        elapsed = [e["elapsed_sec"] for e in events]
        self.assertEqual(len(elapsed), len(events))
        self.assertEqual(elapsed, sorted(elapsed))

        # stdout carries no JSON (that's on stderr) but keeps its normal output:
        # the stable duperemove-compat "net change" line still appears there.
        self.assertNotIn("{", p.stdout)
        self.assertIn("net change in shared extents", p.stdout)

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

    def test_in_memory_run_still_emits_done(self):
        # No --hashfile: the done event is emitted before the (hashfile-only)
        # history record, so an in-memory run reports it too.
        for i in range(3):
            self.write(f"tree/f{i}", os.urandom(65536))
        d = os.path.join(self.work, "tree")
        p = subprocess.run(
            [DUPEREMOVE, "-q", "--io-threads=4", "--progress=json", "-r", d],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        self.assertEqual(p.returncode, 0)
        events = [json.loads(ln) for ln in p.stderr.splitlines() if ln.strip()]
        done = [e for e in events if e.get("event") == "done"]
        self.assertEqual(len(done), 1)
        self.assertEqual(done[0]["files_scanned"], 3)

    @requires_reflink
    def test_tty_stdout_has_no_progress_block(self):
        # On a tty the interactive block must still be suppressed (JSON goes to
        # stderr); only normal human output may reach stdout.
        import pty
        import select

        n = 4
        d = self._dupe_tree(n)
        mfd, sfd = pty.openpty()
        p = subprocess.Popen(
            [DUPEREMOVE, "-q", "--io-threads=4", "--hashfile", self.hf,
             "--progress=json", "-rd", d],
            stdout=sfd, stderr=subprocess.PIPE, close_fds=True)
        os.close(sfd)
        out = b""
        while True:
            if mfd in select.select([mfd], [], [], 0.2)[0]:
                try:
                    chunk = os.read(mfd, 65536)
                except OSError:
                    chunk = b""
                if not chunk:
                    if p.poll() is not None:
                        break
                    continue
                out += chunk
            elif p.poll() is not None:
                break
        err = p.stderr.read().decode()
        p.wait()
        os.close(mfd)

        # No stage line or braille progress bar leaked onto the (tty) stdout.
        self.assertNotIn(b"scanning", out)
        self.assertNotIn("⣿".encode(), out)  # ⣿, a filled bar cell
        # The JSON stream, ending with a done event, is on stderr.
        self.assertIn('"event":"done"', err)

    @unittest.skipIf(os.geteuid() == 0,
                     "running as root defeats the chmod 000 unreadable file")
    def test_errors_go_to_stderr_not_the_report_stream(self):
        """A mid-run error must not land on stdout in JSON mode either (#203).

        The progress printer runs for the whole run here, so every message is
        routed - which used to mean forced onto stdout, mixing diagnostics into
        the stream that carries the human/compat report.
        """
        for i in range(4):
            self.mkdup(f"tree/a{i}", f"tree/b{i}", 200_000)
        os.chmod(self.mkrand("tree/locked.bin", 65536), 0)
        d = os.path.join(self.work, "tree")

        p = subprocess.run(
            [DUPEREMOVE, "--io-threads=4", "--hashfile", self.hf,
             "--progress=json", "-rd", d],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        self.assertEqual(p.returncode, 0)
        self.assertIn("while opening file", p.stderr)
        self.assertNotIn("while opening file", p.stdout)
        self.assertNotIn("locked.bin", p.stdout)

    def test_invalid_progress_value_is_rejected(self):
        p = subprocess.run(
            [DUPEREMOVE, "-r", "--progress=xml", self.work],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        self.assertNotEqual(p.returncode, 0)
        self.assertIn("--progress", p.stderr)
