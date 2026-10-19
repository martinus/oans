"""What a scan covers when a path is spelled or placed unusually (#282).

A root on another filesystem was dropped with no message and exit 0, and a
replay kept dropping it. A relative --hashfile inside the scanned tree was
compared, as typed, against the walk's absolute paths, so the hashfile and its
-wal/-shm were hashed - and changed, and were rehashed - on every run.
"""

import os
import shutil
import subprocess
import tempfile
from harness import DUPEREMOVE, DuperemoveTest, TEST_ROOT


def _can_mount_in_userns():
    try:
        return subprocess.run(["unshare", "-rm", "true"],
                              capture_output=True).returncode == 0
    except OSError:
        return False


class ScanScopeTest(DuperemoveTest):
    def test_a_root_on_another_filesystem_is_reported(self):
        other_parent = "/dev/shm"
        if not os.path.isdir(other_parent) or \
           os.stat(other_parent).st_dev == os.stat(TEST_ROOT).st_dev:
            self.skipTest("no second filesystem at /dev/shm")
        other = tempfile.mkdtemp(prefix="oans-other.", dir=other_parent)
        self.addCleanup(shutil.rmtree, other, True)
        with open(os.path.join(other, "f"), "wb") as f:
            f.write(os.urandom(8000))
        self.mkrand("tree/a", 8000)

        self.dm("-r", self.path("tree"), other)
        self.assertEqual(2, self.rc, self.out)
        self.assertIn("on another filesystem", self.out)
        self.assertEqual(1, self.hf_count("files"), "the first root is scanned")

    def test_a_mount_below_the_root_is_not_an_unsupported_filesystem(self):
        """A run locked on btrfs asked every non-btrfs mount below a root for
        its UUID. A squashfs loop mount has none, so each one printed a
        libblkid error at default verbosity, and every such mount was counted
        as an unsupported filesystem with nothing under -v to say which.
        ramfs has no UUID either and can be mounted without root.
        """
        if not _can_mount_in_userns():
            self.skipTest("no unprivileged mount namespace")
        self.mkrand("tree/a", 8000)
        os.makedirs(self.path("tree/mnt"))
        script = 'mount -t ramfs none "$1" && shift && exec "$@"'
        proc = subprocess.run(
            ["unshare", "-rm", "sh", "-c", script, "sh",
             self.path("tree/mnt"), DUPEREMOVE, "-rv", "--io-threads=4",
             "--hashfile", self.hf, self.path("tree")],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        self.out, self.rc = proc.stdout, proc.returncode
        self.assertNoSanitizerReport(self.out)
        self.assertDmOk()
        self.assertIn(f"Skipping {self.path('tree/mnt')}: another filesystem "
                      "is mounted there", self.out)
        self.assertNotIn("unsupported filesystem", self.out)
        self.assertEqual(1, self.hf_count("files"))

    def test_a_hashfile_locked_on_another_filesystem_names_both(self):
        """One line naming both filesystems: the message used to be five
        separate prints, and on a tty each one redraws the live block (#179).
        """
        self.mkrand("tree/a", 8000)
        self.dm("-r", self.path("tree"))
        self.assertEqual(0, self.rc, self.out)
        real = self.hf_query("select keyval from config "
                             "where keyname = 'fs_uuid'")[0][0]
        if isinstance(real, bytes):
            real = real.decode()
        other = "01234567-89ab-cdef-0123-456789abcdef"
        self.hf_exec("update config set keyval = ? where keyname = 'fs_uuid'",
                     (other,))

        self.dm("-r", self.path("tree"))
        self.assertNotEqual(0, self.rc, self.out)
        self.assertIn(f"lives on fs {real} while the hashfile is locked on "
                      f"fs {other}.\n", self.out)

    def test_a_relative_hashfile_inside_the_tree_is_not_scanned(self):
        self.mkrand("tree/a", 8000)
        hf = os.path.relpath(self.path("tree/h.db"))
        for _ in range(2):              # the sidecars exist from the second run
            self.dm("-r", self.path("tree"), "--hashfile", hf, hashfile=False)
            self.assertEqual(0, self.rc, self.out)
        self.hf = self.path("tree/h.db")
        self.assertEqual([("a",)], self.hf_query(
            "select replace(filename, rtrim(filename, "
            "replace(filename, '/', '')), '') from files"))
