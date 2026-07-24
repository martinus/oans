"""Byte-weighted dedupe progress: work_done_bytes / work_total_bytes.

The dedupe bar tracks the kernel byte-verify volume (de_len*(de_num_dupes-1)
per group), not a fuzzy group count. The total is computed exactly up front by
SQL, ticked every <=32 MiB ioctl round, credited in a settle-up lump for
skipped work, and exposed raw on the --progress=json stream. These tests assert
the total is byte-exact for the default whole-file config, that the raw fields
never regress, that the upfront total is stable across passes, and that skipped
work is still fully credited (settlement). Requires a reflink-capable fs.
"""

import json
import os
import subprocess

from harness import DuperemoveTest, requires_reflink, DUPEREMOVE

MiB = 1024 * 1024


class ProgressBytesTest(DuperemoveTest):
    def _run(self, *args, env=None):
        """Run oans -rd with --progress=json; return (proc, events)."""
        cmd = [DUPEREMOVE, "-q", "--io-threads=4", "--hashfile", self.hf,
               "--progress=json", *args]
        run_env = dict(os.environ, **env) if env else None
        p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                           text=True, env=run_env)
        events = [json.loads(ln) for ln in p.stderr.splitlines() if ln.strip()]
        return p, events

    @staticmethod
    def _dedupe_evs(events):
        return [e for e in events if e.get("phase") == "dedupe"]

    @requires_reflink
    def test_work_total_is_byte_exact_default_config(self):
        # One 8 MiB pair (work 8 MiB) + one 4 MiB triple (work 8 MiB) => 16 MiB.
        self.mkdup("tree/p_a", "tree/p_b", 8 * MiB)
        d = os.urandom(4 * MiB)
        for n in ("a", "b", "c"):
            self.write(f"tree/t_{n}", d)
        self.sync()

        p, events = self._run("-r" + "d", self.path("tree"))
        self.assertEqual(p.returncode, 0)
        dedupe = self._dedupe_evs(events)
        self.assertTrue(dedupe)

        expected = 8 * MiB + 8 * MiB
        totals = [e["work_total_bytes"] for e in dedupe]
        # Whole-file groups are byte-exact: the upfront total hits it exactly.
        self.assertEqual(max(totals), expected)

        # Post-settlement, the last dedupe record has done == total.
        last = dedupe[-1]
        self.assertEqual(last["work_done_bytes"], last["work_total_bytes"])
        self.assertEqual(last["work_done_bytes"], expected)

    @requires_reflink
    def test_raw_fields_never_regress(self):
        for i in range(6):
            self.mkdup(f"tree/a{i}", f"tree/b{i}", 512 * 1024)
        self.sync()

        _p, events = self._run("-rd", self.path("tree"))
        dedupe = self._dedupe_evs(events)
        self.assertTrue(dedupe)

        done = [e["work_done_bytes"] for e in dedupe]
        total = [e["work_total_bytes"] for e in dedupe]
        # done is non-decreasing; total never shrinks (0 -> upfront sum, held).
        self.assertEqual(done, sorted(done), f"work_done regressed: {done}")
        self.assertEqual(total, sorted(total), f"work_total shrank: {total}")

    @requires_reflink
    def test_upfront_total_stable_across_passes(self):
        # Small passes + small batchsize so the work spans several generations;
        # the byte total is computed once up front and must not change per pass.
        env = {"DUPEREMOVE_FILES_PER_PASS": "8"}
        sizes = []
        for i in range(20):
            n = 32 * 1024 + i * 4096  # distinct sizes => distinct groups
            self.mkdup(f"tree/a{i:02d}", f"tree/z{i:02d}", n)
            sizes.append(n)
        self.sync()

        _p, events = self._run("-rd", "-B", "4", self.path("tree"), env=env)
        dedupe = self._dedupe_evs(events)
        self.assertTrue(dedupe)

        expected = sum(sizes)  # each pair: size * (2 - 1)
        nonzero = {e["work_total_bytes"] for e in dedupe
                   if e["work_total_bytes"]}
        # Exactly one nonzero total, equal to the whole-file work sum.
        self.assertEqual(nonzero, {expected}, nonzero)

        done = [e["work_done_bytes"] for e in dedupe]
        self.assertEqual(done, sorted(done), f"work_done regressed: {done}")
        self.assertEqual(dedupe[-1]["work_done_bytes"], expected)

    @requires_reflink
    def test_incremental_noop_rerun_zero_work(self):
        for i in range(4):
            self.mkdup(f"tree/a{i}", f"tree/b{i}", 256 * 1024)
        self.sync()
        self._run("-rd", self.path("tree"))  # first pass does the work

        # Second run: nothing new to dedupe. No crash, zero work, zero groups.
        p, events = self._run("-rd", self.path("tree"))
        self.assertEqual(p.returncode, 0)
        dedupe = self._dedupe_evs(events)
        self.assertTrue(dedupe)
        self.assertTrue(all(e["work_total_bytes"] == 0 for e in dedupe))
        self.assertTrue(all(e["work_done_bytes"] == 0 for e in dedupe))
        done = [e for e in events if e.get("event") == "done"]
        self.assertEqual(len(done), 1)
        self.assertEqual(done[0]["groups_deduped"], 0)

    @requires_reflink
    def test_skip_work_is_fully_credited(self):
        # Dedupe once, then re-run against a FRESH hashfile: the tree is already
        # shared on disk, so nearly all work is skip-credited (already-shared /
        # clean_deduped) rather than done by the kernel. Settlement must still
        # bring work_done up to work_total.
        for i in range(5):
            self.mkdup(f"tree/a{i}", f"tree/b{i}", 1 * MiB)
        self.sync()
        self._run("-rd", self.path("tree"))
        self.sync()

        # Fresh hashfile forces a full re-analysis of the (already shared) tree.
        os.unlink(self.hf)
        for sidecar in (self.hf + "-wal", self.hf + "-shm"):
            if os.path.exists(sidecar):
                os.unlink(sidecar)

        _p, events = self._run("-rd", self.path("tree"))
        dedupe = self._dedupe_evs(events)
        self.assertTrue(dedupe)
        expected = 5 * MiB  # five pairs, 1 MiB work each
        last = dedupe[-1]
        self.assertEqual(last["work_total_bytes"], expected)
        self.assertEqual(last["work_done_bytes"], expected,
                         "skipped work must be settled up to the total")
