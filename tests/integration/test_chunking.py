"""Intra-file chunked hashing (#88).

Chunking is a pure throughput knob: splitting a large file across IO threads
must produce byte-for-byte the same hashfile (file digest, blocks and extents)
as an unchunked scan, and must still dedupe. These tests force chunking on with
a small --chunksize; --chunksize=0 is the unchunked baseline.
"""

import os
from harness import (
    DuperemoveTest, requires_reflink, requires_btrfs, fiemap_extents,
)


_BLOCK = 128 * 1024   # oans' default block size; extent ends land on multiples


def _fragmented(path, size=4 * 1024 * 1024):
    """Write `size` random bytes, then rewrite alternate 128 KiB regions in place.
    On btrfs (copy-on-write) each rewrite lands in a fresh physical extent, so the
    file splits into many block-aligned extents - the split points the chunk
    planner needs. The rewrites put the same bytes back, so the content (and thus
    every digest) is unchanged. Only reliable on btrfs (hence @requires_btrfs);
    append+fsync coalesces on some btrfs versions, so we assert the file really
    fragmented rather than let a chunked scan silently fall back to whole-file."""
    blob = os.urandom(size)
    with open(path, "wb") as f:
        f.write(blob)
        f.flush()
        os.fsync(f.fileno())
    fd = os.open(path, os.O_WRONLY)
    try:
        for off in range(0, size, 2 * _BLOCK):
            os.pwrite(fd, blob[off:off + _BLOCK], off)
            os.fsync(fd)
    finally:
        os.close(fd)
    assert len(fiemap_extents(path)) > 1, \
        f"{path} did not fragment; the chunk path would not be exercised"
    return blob


@requires_btrfs
class ChunkingTest(DuperemoveTest):
    def _rm_hashfile(self):
        for suffix in ("", "-wal", "-shm"):
            try:
                os.remove(self.hf + suffix)
            except FileNotFoundError:
                pass

    def _blocks(self):
        """(loff, digest) of every stored block, in scan order."""
        return self.hf_query(
            "select loff, quote(digest) from blocks order by fileid, loff")

    def test_chunked_scan_matches_whole(self):
        # Compare an unchunked and a chunked scan of the SAME file (same fiemap),
        # so file digest and extents must match exactly. Block-level matching is
        # enabled so block hashes are stored and compared too.
        _fragmented(self.path("tree/big"))
        opts = "--dedupe-options=partial"

        self.scan(self.path("tree"), "--chunksize=0", opts)
        self.assertDmOk()
        whole_digest = self.hf_scalar("select quote(digest) from files")
        whole_extents = self.extents_fingerprint()
        whole_blocks = self._blocks()
        self.assertGreater(len(whole_blocks), 0, "block hashes were stored")

        self._rm_hashfile()

        # 1 MiB target over 512 KiB extents -> several chunks.
        self.scan(self.path("tree"), "--chunksize=1M", opts)
        self.assertDmOk()
        self.assertEqual(
            whole_digest, self.hf_scalar("select quote(digest) from files"),
            "chunked file digest matches the whole-file scan")
        self.assertEqual(whole_extents, self.extents_fingerprint(),
                         "chunked extents match the whole-file scan")
        self.assertEqual(whole_blocks, self._blocks(),
                         "chunked block hashes match the whole-file scan")

    def test_chunked_noop_rescan(self):
        _fragmented(self.path("tree/big"))
        self.scan(self.path("tree"), "--chunksize=1M")
        before = self.files_fingerprint()
        self.scan(self.path("tree"), "--chunksize=1M")
        self.assertDmOk()
        self.assertEqual(before, self.files_fingerprint(),
                         "a no-op chunked rescan changes nothing")

    @requires_reflink
    def test_chunked_file_dedupes(self):
        blob = _fragmented(self.path("tree/a"))
        self.write("tree/b", blob)   # identical, independently stored
        self.sync()

        self.dedupe(self.path("tree"), "--chunksize=1M")
        self.assertDmOk()
        self.assertShared(self.path("tree/a"), self.path("tree/b"),
                          "identical files dedupe even when chunk-scanned")
