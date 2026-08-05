"""Self-describing hashfile: a run stores its scan-shaping options, roots and
exclude patterns, so a later bare `oans --hashfile=X` (no file arguments)
replays it.

Covers the option/root/exclude round-trip, last-run-wins overwrite semantics,
and the two guards: refuse a bare run with nothing stored, and refuse (without
pruning) when none of the stored roots still exist.
"""

import os
import shutil

from harness import DuperemoveTest, requires_reflink


class SelfDescribingConfigTest(DuperemoveTest):
    """Config storage/replay that does not depend on real dedupe."""

    def _opt(self, name):
        return self.hf_scalar(
            "select keyval from config where keyname=?", (name,))

    def _seed_tree(self, name="tree", contents=b"x" * 4096):
        """Create <name>/ holding one file; return the dir's absolute path."""
        d = self.path(name)
        os.makedirs(d)
        self.write(f"{name}/a", contents)
        return d

    def test_run_stores_options_and_roots(self):
        d = self._seed_tree()

        self.dm("-r", "--min-filesize=2048", d)
        self.assertDmOk()

        self.assertEqual(self._opt("scan_config"), 1)
        self.assertEqual(self._opt("opt_recurse"), 1)
        self.assertEqual(self._opt("opt_run_dedupe"), 0)
        self.assertEqual(self._opt("opt_min_filesize"), 2048)
        roots = [r[0] for r in self.hf_query("select path from scan_roots")]
        self.assertEqual(roots, [os.path.realpath(d)])

    def test_last_run_wins_overwrites(self):
        d = self._seed_tree()

        self.dm("-rd", d)
        self.assertEqual(self._opt("opt_run_dedupe"), 1)

        # A second run with different options replaces the stored set.
        self.dm("-r", "--dedupe-options=nosame", d)
        self.assertEqual(self._opt("opt_run_dedupe"), 0)
        self.assertEqual(self._opt("opt_dedupe_same_file"), 0)

    def test_all_sticky_options_round_trip(self):
        # Backstop: every scan-shaping option a run uses must be persisted, so
        # adding a new one without wiring persist_scan_config fails here.
        d = self._seed_tree(contents=b"x" * 16384)
        self.dm("-rd", "--skip-zeroes", "--min-filesize=8192",
                "--dedupe-options=nosame,only_whole_files", d)
        self.assertDmOk()
        self.assertEqual(self._opt("opt_run_dedupe"), 1)
        self.assertEqual(self._opt("opt_recurse"), 1)
        self.assertEqual(self._opt("opt_skip_zeroes"), 1)
        self.assertEqual(self._opt("opt_only_whole_files"), 1)
        self.assertEqual(self._opt("opt_dedupe_same_file"), 0)   # nosame
        self.assertEqual(self._opt("opt_do_block_hash"), 0)      # not requested
        self.assertEqual(self._opt("opt_min_filesize"), 8192)

    def test_bare_run_without_stored_config_errors(self):
        # Fresh hashfile, no arguments: nothing to replay.
        self.dm()
        self.assertNotEqual(self.rc, 0)
        self.assertIn("no stored scan configuration", self.out)

    def test_all_roots_missing_refuses_and_keeps_rows(self):
        d = self._seed_tree()
        self.dm("-r", d)
        rows_before = self.hf_count("files")
        self.assertGreater(rows_before, 0)

        shutil.rmtree(d)          # the only stored root disappears

        self.dm()                 # bare replay
        self.assertNotEqual(self.rc, 0)
        self.assertIn("refusing to run", self.out)
        # The guard must not let the prune wipe the hashfile.
        self.assertEqual(self.hf_count("files"), rows_before)

    def test_some_roots_missing_skips_and_keeps_going(self):
        # Two roots stored; delete one. The run must warn, scan the survivor,
        # and not crash (regression for a double-free while compacting roots).
        os.makedirs(self.path("one"))
        os.makedirs(self.path("two"))
        self.write("one/a", b"a" * 4096)
        self.write("two/b", b"b" * 4096)
        self.dm("-r", self.path("one"), self.path("two"))
        self.assertEqual(self.hf_count("scan_roots"), 2)

        shutil.rmtree(self.path("one"))     # one of two roots vanishes
        self.dm()
        # Exit 2 = "completed, but covered less than asked" (#146). Asserting
        # the exact code rather than just non-zero keeps this a crash
        # regression too: a double-free would show up as a signal, not as 2.
        self.assertEqual(self.rc, 2)
        self.assertIn("no longer exists", self.out)
        # A replay does not rewrite the stored config, so the temporarily
        # missing root is kept and retried next time (e.g. an unmounted drive),
        # not silently forgotten.
        roots = sorted(r[0] for r in self.hf_query("select path from scan_roots"))
        self.assertEqual(roots, sorted([os.path.realpath(self.path("one")),
                                        os.path.realpath(self.path("two"))]))

    def test_stats_shows_stored_config(self):
        os.makedirs(self.path("tree/skip"))
        self.write("tree/a", b"a" * 8192)
        self.dm("-rd", "--min-filesize=4096",
                "--exclude", self.path("tree/skip"), self.path("tree"))

        self.dm("--stats")
        self.assertDmOk()
        self.assertIn("stored scan", self.out)
        self.assertIn("-r", self.out)
        self.assertIn("-d", self.out)
        self.assertIn("--min-filesize=4096", self.out)
        self.assertIn(os.path.realpath(self.path("tree")), self.out)
        self.assertIn(os.path.realpath(self.path("tree/skip")), self.out)

    def test_excludes_round_trip(self):
        os.makedirs(self.path("tree/keep"))
        os.makedirs(self.path("tree/skip"))
        self.write("tree/keep/a", b"a" * 4096)
        self.write("tree/skip/b", b"b" * 4096)

        self.dm("-r", "--exclude", self.path("tree/skip"), self.path("tree"))
        stored = [r[0] for r in self.hf_query("select pattern from scan_excludes")]
        self.assertEqual(stored, [os.path.realpath(self.path("tree/skip"))])
        self.assertNotIn("skip", self.hf_query("select filename from files").__repr__())

        # A new file under the excluded dir must stay out on replay.
        self.write("tree/skip/c", b"c" * 4096)
        self.dm()
        self.assertDmOk()
        names = [r[0] for r in self.hf_query("select filename from files")]
        self.assertTrue(all("/skip/" not in n for n in names), names)


@requires_reflink
class SelfDescribingReplayDedupeTest(DuperemoveTest):
    """A bare replay must actually re-scan the stored root and dedupe."""

    def test_replay_dedupes_new_duplicates(self):
        d = self.path("tree")
        os.makedirs(d)
        a, b = self.mkdup("tree/a", "tree/b", 1 << 20)

        # First run stores "-rd <tree>" and dedupes the initial pair.
        self.dm("-rd", d)
        self.assertShared(a, b)

        # Add a brand-new duplicate pair, then replay with no arguments.
        c, e = self.mkdup("tree/c", "tree/e", 1 << 20)
        self.assertNotShared(c, e)

        self.dm()                 # bare: replays -r -d <tree>
        self.assertDmOk()
        self.assertShared(c, e)   # proves the stored root was re-scanned + deduped
