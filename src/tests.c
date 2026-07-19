#include <stdbool.h>
#include <sys/stat.h>

#include "minunit.h"
#include "rbtree.c"

#include "opt.c"
#include "util.c"
#include "debug.c"
#include "csum.c"
#include "threads.c"
#include "btrfs-util.c"
#include "file_scan.c"
#include "filerec.c"
#include "dbfile.c"
#include "hash-tree.c"
#include "results-tree.c"
#include "list_sort.c"
#include "find_dupes.c"
#include "memstats.c"
#include "fiemap.c"
#include "progress.c"
#include "storage.c"


unsigned int blocksize = DEFAULT_BLOCKSIZE;
static char *exec_path;

MU_TEST(test_is_block_zeroed) {
	blocksize = 100;
	char block[100] = {0,};
	// Actual zeroed block
	mu_check(is_block_zeroed(&block) == true);

	// Block has the same content, but not zeroed
	memset(block, 1, 100);
	mu_check(is_block_zeroed(&block) == false);

	// Block do not have the same content
	block[50] = 50;
	mu_check(is_block_zeroed(NULL) == false);
}

MU_TEST(test_block_len) {
	struct file_block block;
	struct filerec file;

	block.b_file = &file;

	// First block of the file
	file.size = 10 * 1024 * 1024;
	block.b_loff = 0;
	mu_check(block_len(&block) == blocksize);

	// block in the middle of the file, unaligned
	block.b_loff = 1;
	mu_check(block_len(&block) == blocksize);

	// block in the middle of the file, aligned
	block.b_loff = blocksize * 10;
	mu_check(block_len(&block) == blocksize);

	// block at the end of the file, which is aligned
	file.size = blocksize * 10;
	block.b_loff = blocksize * 9;
	mu_check(block_len(&block) == blocksize);

	// block at the end of the file, which is unaligned
	unsigned int extra = 10;
	file.size = blocksize * 10 + extra;
	block.b_loff = blocksize * 10;
	mu_check(block_len(&block) == extra);

	// loff is passed filesize
	file.size = blocksize * 10 + extra;
	block.b_loff = blocksize * 15;
	mu_check(block_len(&block) == 0);
}

MU_TEST(test_is_file_renamed) {
	char *new_path = "/tmp/somefile";
	char *path_in_db = "/tmp/somefile";

	mu_check(is_file_renamed(path_in_db, new_path) == false);

	path_in_db = "/tmp/anotherfile";
	mu_check(is_file_renamed(path_in_db, new_path) == true);

	/*
	 * Diffents path but the old one still exists.
	 * We use our own file to simulate a hard link
	 */
	mu_check(is_file_renamed(exec_path, new_path) == false);
}

MU_TEST(test_seen_inode) {
	/*
	 * The scan skips a dirent whose (ino, subvol) was already written this
	 * scan (a further hardlink to one inode), which is how the batched
	 * writer avoids re-storing - and corrupting - a pending filerec. The
	 * match must be exact on both fields: a hash collision that reported a
	 * distinct inode as "seen" would silently drop a real file.
	 */
	seen_inodes_init();
	mu_check(seen_slots != NULL);

	mu_check(seen_inode(42, 7) == false);
	mark_inode_seen(42, 7);
	mu_check(seen_inode(42, 7) == true);	/* the hardlink is skipped */

	/* Same ino in another subvol, or another ino here, is a different file
	 * and must not be reported as seen. */
	mu_check(seen_inode(42, 8) == false);
	mu_check(seen_inode(43, 7) == false);

	/* Values whose 64-bit fields are swapped must not alias each other. */
	mark_inode_seen(7, 42);
	mu_check(seen_inode(7, 42) == true);
	mu_check(seen_inode(42, 7) == true);

	/* Stress the grow/rehash path (initial capacity is 1024): insert many
	 * distinct keys, then verify exact membership survives the resizes. */
	for (uint64_t n = 0; n < 5000; n++)
		mark_inode_seen(1000 + n, n & 3);
	for (uint64_t n = 0; n < 5000; n++)
		mu_check(seen_inode(1000 + n, n & 3) == true);
	mu_check(seen_inode(1000 + 5000, 0) == false);	/* never inserted */
	mu_check(seen_inode(999, 0) == false);

	seen_inodes_free();
}

MU_TEST(test_get_extent) {
	/* Three data extents with holes between them:
	 * [0, 4k)   hole   [8k, 12k)   hole   [16k, 20k) */
	unsigned int n = 3;
	struct fiemap *fm = calloc(1, sizeof(*fm) +
				   n * sizeof(struct fiemap_extent));

	fm->fm_mapped_extents = n;
	fm->fm_extents[0].fe_logical = 0;     fm->fm_extents[0].fe_length = 4096;
	fm->fm_extents[1].fe_logical = 8192;  fm->fm_extents[1].fe_length = 4096;
	fm->fm_extents[2].fe_logical = 16384; fm->fm_extents[2].fe_length = 4096;

	/* Plain lookups (no cursor). */
	mu_check(get_extent(fm, 0, NULL) == &fm->fm_extents[0]);
	mu_check(get_extent(fm, 4095, NULL) == &fm->fm_extents[0]);
	mu_check(get_extent(fm, 4096, NULL) == &fm->fm_extents[1]); /* in hole -> next */
	mu_check(get_extent(fm, 8192, NULL) == &fm->fm_extents[1]);
	mu_check(get_extent(fm, 16384, NULL) == &fm->fm_extents[2]);
	mu_check(get_extent(fm, 20480, NULL) == NULL);             /* past EOF */

	/* A resume cursor must give identical answers for a monotonically
	 * increasing sequence of offsets (the scan access pattern). */
	unsigned int cur = 0;
	size_t offs[] = { 0, 4095, 4096, 8192, 12000, 16384, 19000 };
	for (unsigned int i = 0; i < ARRAY_SIZE(offs); i++)
		mu_check(get_extent(fm, offs[i], &cur) ==
			 get_extent(fm, offs[i], NULL));

	/* A stale cursor pointing past the target must still be correct
	 * (get_extent falls back to a full scan). */
	cur = 2;
	mu_check(get_extent(fm, 0, &cur) == &fm->fm_extents[0]);
	cur = 2;
	mu_check(get_extent(fm, 8192, &cur) == &fm->fm_extents[1]);

	free(fm);
}

MU_TEST(test_sanitize_ctrl) {
	char out[64];

	/* Plain ASCII and legitimate multi-byte UTF-8 pass through unchanged. */
	sanitize_ctrl("plain.txt", out, sizeof(out));
	mu_check(strcmp(out, "plain.txt") == 0);
	sanitize_ctrl("café-Β.txt", out, sizeof(out));   /* é=C3A9, Β=CE92 */
	mu_check(strcmp(out, "café-Β.txt") == 0);

	/* C0 control and DEL become '?'. */
	sanitize_ctrl("a\tb\nc\x7f", out, sizeof(out));
	mu_check(strcmp(out, "a?b?c?") == 0);

	/* C1 control U+009F (UTF-8 C2 9F) becomes a single '?' (#353). */
	sanitize_ctrl("Te\xc2\x9ft", out, sizeof(out));
	mu_check(strcmp(out, "Te?t") == 0);

	/* Truncation stays NUL-terminated and within bounds. */
	char small[4];
	sanitize_ctrl("abcdef", small, sizeof(small));
	mu_check(strcmp(small, "abc") == 0);
}

MU_TEST(test_storage_recommend_io_threads) {
	struct storage_profile p;

	/* SSD / non-rotational: keep the full CPU-capped default (cap 8). */
	p = (struct storage_profile){ .rotational = false,
		.rotational_known = true, .num_devices = 1 };
	mu_check(storage_recommend_io_threads(&p, 4) == 4);
	mu_check(storage_recommend_io_threads(&p, 32) == 8);	/* capped */

	/* Unknown media falls back to the same default, never fewer. */
	p = (struct storage_profile){ .rotational = false,
		.rotational_known = false, .num_devices = 1 };
	mu_check(storage_recommend_io_threads(&p, 16) == 8);
	mu_check(storage_recommend_io_threads(&p, 2) == 2);

	/* Single spinning disk: few concurrent readers (seek-bound), max 4. */
	p = (struct storage_profile){ .rotational = true,
		.rotational_known = true, .num_devices = 1 };
	mu_check(storage_recommend_io_threads(&p, 32) == 4);
	mu_check(storage_recommend_io_threads(&p, 2) == 2);	/* fewer cores wins */

	/* HDD pool: ~2 readers per spindle, still capped at 8 and by cores. */
	p = (struct storage_profile){ .rotational = true,
		.rotational_known = true, .num_devices = 2 };
	mu_check(storage_recommend_io_threads(&p, 32) == 4);
	p.num_devices = 4;
	mu_check(storage_recommend_io_threads(&p, 32) == 8);	/* 2*4, capped 8 */
	mu_check(storage_recommend_io_threads(&p, 6) == 6);	/* cores limit */

	/* Degenerate CPU count still yields at least one thread. */
	p = (struct storage_profile){ .rotational = true,
		.rotational_known = true, .num_devices = 1 };
	mu_check(storage_recommend_io_threads(&p, 0) == 1);
}

MU_TEST_SUITE(test_suite) {
	MU_RUN_TEST(test_is_block_zeroed);
	MU_RUN_TEST(test_block_len);
	MU_RUN_TEST(test_is_file_renamed);
	MU_RUN_TEST(test_seen_inode);
	MU_RUN_TEST(test_get_extent);
	MU_RUN_TEST(test_sanitize_ctrl);
	MU_RUN_TEST(test_storage_recommend_io_threads);
}

int main(int argc [[maybe_unused]], char *argv[]) {
	exec_path = argv[0];
	MU_RUN_SUITE(test_suite);
	MU_REPORT();
	return MU_EXIT_CODE;
}
