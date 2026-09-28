"""A worker thread that cannot be started (#277).

g_thread_pool_push() reports an error only when it could not start a new
thread, and queues the item all the same. Both pools read that error as "not
queued": the block search freed an item a worker still held and dropped its
outstanding count twice, so a later wait returned early or hung for good, and
the dedupe phase aborted. systemd's TasksMax and RLIMIT_NPROC are the ways a
scheduled run gets there.

DUPEREMOVE_POOL_SPAWN_FAIL reports that error after every real push, which is
exactly what GLib does when a spawn fails and a thread is still running.
"""

import os
from harness import DuperemoveTest, requires_reflink

HOOK = {"DUPEREMOVE_POOL_SPAWN_FAIL": "1"}
WARNING = "could not start another worker thread"


@requires_reflink
class PoolSpawnFailTest(DuperemoveTest):
    def tree(self):
        for i in range(6):
            self.mkdup(f"tree/a{i}", f"tree/b{i}", 256 * 1024 + 4096 * i)
        return self.path("tree")

    def run_with_hook(self, *args):
        # The old reading hung the block search for good: fail, not hang.
        self.dm("-rd", self.tree(), *args, env=HOOK, timeout=120)
        self.assertEqual(0, self.rc, self.out)
        self.assertEqual(1, self.out.count(WARNING), "warned, once")
        for i in range(6):
            self.assertShared(self.path(f"tree/a{i}"), self.path(f"tree/b{i}"))

    def test_the_dedupe_pool_carries_on(self):
        self.run_with_hook()

    def test_the_block_search_waits_for_what_it_queued(self):
        self.run_with_hook("--dedupe-options=partial")
