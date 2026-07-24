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
#include "longpath.c"


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

	/* Cursor already on the answer while loff sits in the hole just before
	 * it (the sparse scan resuming after a skipped hole): must resolve to
	 * that same extent, so the O(1) resume holds instead of rescanning. */
	cur = 1;
	mu_check(get_extent(fm, 4096, &cur) == &fm->fm_extents[1]);
	mu_check(cur == 1);

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

MU_TEST(test_scan_bucket) {
	/* Boundaries: <1 MiB => 0, then one bucket per power of two above 1 MiB. */
	mu_check(scan_bucket(0) == 0);
	mu_check(scan_bucket(1) == 0);
	mu_check(scan_bucket((1u << 20) - 1) == 0);		/* just under 1 MiB */
	mu_check(scan_bucket(1u << 20) == 1);			/* 1 MiB (2^20) */
	mu_check(scan_bucket((1u << 21) - 1) == 1);		/* just under 2 MiB */
	mu_check(scan_bucket(1u << 21) == 2);			/* 2 MiB */
	mu_check(scan_bucket(1u << 22) == 3);			/* 4 MiB */
	mu_check(scan_bucket(1u << 24) == 5);			/* 16 MiB */
	mu_check(scan_bucket(100ull << 20) == 7);		/* 100 MiB (2^26 top bit) */
	mu_check(scan_bucket(8ull << 30) == 14);		/* 8 GiB (2^33) */
	/* Even the largest possible size stays a valid bucket index. */
	mu_check(scan_bucket(~0ull) == 44);			/* 2^63 top bit */
	mu_check(scan_bucket(~0ull) < SCAN_NBUCKETS);
}

MU_TEST(test_scan_workq_priority) {
	/*
	 * A free thread must take the largest-bucket work first, FIFO (walk order)
	 * within a bucket. Drive scan_workq_push/pop directly (no workers): with
	 * items queued, pop() returns immediately in dispatch order.
	 */
	memset(&scan_workq, 0, sizeof(scan_workq));

	struct file_to_scan files[] = {
		{ .filesize = 512u << 10, .file_position = 1 },	/* 512 KiB -> b0 */
		{ .filesize = 8u << 20,   .file_position = 2 },	/* 8 MiB   -> b4 */
		{ .filesize = 2u << 20,   .file_position = 3 },	/* 2 MiB   -> b2 */
		{ .filesize = 8u << 20,   .file_position = 4 },	/* 8 MiB   -> b4 */
		{ .filesize = 100u << 20, .file_position = 5 },	/* 100 MiB -> b7 */
	};
	for (unsigned int i = 0; i < G_N_ELEMENTS(files); i++)
		scan_workq_push(&files[i]);

	/* Biggest bucket first; within b4, FIFO keeps pos2 before pos4. */
	struct file_to_scan *f;
	f = scan_workq_pop(&scan_workq); mu_check(f->file_position == 5);	/* b7 */
	f = scan_workq_pop(&scan_workq); mu_check(f->file_position == 2);	/* b4 */
	f = scan_workq_pop(&scan_workq); mu_check(f->file_position == 4);	/* b4 */
	f = scan_workq_pop(&scan_workq); mu_check(f->file_position == 3);	/* b2 */
	f = scan_workq_pop(&scan_workq); mu_check(f->file_position == 1);	/* b0 */

	/* Empty + draining => pop returns NULL (worker would exit). */
	scan_workq.draining = true;
	mu_check(scan_workq_pop(&scan_workq) == NULL);

	memset(&scan_workq, 0, sizeof(scan_workq));
}

/* Within eps of expected. */
static bool near(double got, double want, double eps)
{
	double e = got - want;
	return e < eps && e > -eps;
}

MU_TEST(test_scan_eta) {
	const uint64_t GiB = 1ull << 30, W = 1ull << 30;   /* 1 GiB per-file weight */

	/* Weighted progress: work = bytes + W*files, ETA = elapsed*(total-done)/done. */

	/* Nothing scanned yet -> no estimate. */
	mu_check(scan_eta_seconds(0, 0, 4 * GiB, 0, W, 10.0) < 0.0);

	/* Pure files (weight is what counts): 1 of 4 files done in 10 s -> 30 s left.
	 * done_work = W, total_work = 4*W, eta = 10*(4-1)/1. */
	mu_check(near(scan_eta_seconds(0, 1, 0, 4, W, 10.0), 30.0, 1e-6));

	/* Pure bytes: 2 of 8 GiB in 12 s -> 36 s. */
	mu_check(near(scan_eta_seconds(2 * GiB, 0, 8 * GiB, 0, W, 12.0), 36.0, 1e-6));

	/* Mixed, weight ties them together: done_work = 1 GiB + W*1 = 2 GiB,
	 * total_work = 1 GiB + W*5 = 6 GiB, eta = 10*(6-2)/2 = 20 s. */
	mu_check(near(scan_eta_seconds(GiB, 1, GiB, 5, W, 10.0), 20.0, 1e-6));

	/* A larger weight up-weights the remaining files, raising the estimate:
	 * done_work = 1 GiB + 2 GiB*1 = 3 GiB, total = 1 GiB + 2 GiB*5 = 11 GiB,
	 * eta = 10*(11-3)/3. */
	mu_check(near(scan_eta_seconds(GiB, 1, GiB, 5, 2 * GiB, 10.0),
		      10.0 * 8.0 / 3.0, 1e-6));

	/* Done >= total -> 0, never negative or a fallback signal. */
	mu_check(near(scan_eta_seconds(4 * GiB, 4, 4 * GiB, 4, W, 10.0), 0.0, 1e-6));
}

MU_TEST(test_group_u64) {
	char b[28];

	/* Under 1000: unchanged. */
	group_u64_snprintf(0, b, sizeof(b));
	mu_check(strcmp(b, "0") == 0);
	group_u64_snprintf(7, b, sizeof(b));
	mu_check(strcmp(b, "7") == 0);
	group_u64_snprintf(999, b, sizeof(b));
	mu_check(strcmp(b, "999") == 0);

	/* Separators every three digits from the right. */
	group_u64_snprintf(1000, b, sizeof(b));
	mu_check(strcmp(b, "1,000") == 0);
	group_u64_snprintf(12000, b, sizeof(b));
	mu_check(strcmp(b, "12,000") == 0);
	group_u64_snprintf(2505166, b, sizeof(b));
	mu_check(strcmp(b, "2,505,166") == 0);

	/* Full width UINT64_MAX still fits the 28-byte buffer. */
	group_u64_snprintf(18446744073709551615ull, b, sizeof(b));
	mu_check(strcmp(b, "18,446,744,073,709,551,615") == 0);

	/* Truncation stays NUL-terminated and within bounds. */
	char small[4];
	group_u64_snprintf(2505166, small, sizeof(small));
	mu_check(small[3] == '\0' && strlen(small) <= 3);
}

/*
 * longpath: reach a file whose absolute path exceeds PATH_MAX. Builds a chain
 * of 255-char directories (via incremental chdir, since the leaf's own path is
 * too long to pass to a syscall) under a /tmp temp dir, then checks that
 * longpath_open/longpath_stat reach the deep leaf that a plain open/stat could
 * not. Runs on tmpfs (no reflink needed); best-effort teardown climbs back out.
 */
#define LP_COMP_LEN 255

static void lp_fill(char *buf, char c, int n)
{
	memset(buf, c, n);
	buf[n] = '\0';
}

static int lp_make_deep(char *absdir, size_t abscap, char *base_out,
			size_t base_cap, int *out_levels, const char *victim,
			const char *contents)
{
	char comp[LP_COMP_LEN + 1];
	char base[] = "/tmp/oans-longpath-XXXXXX";
	size_t len = strlen(base);
	int levels = 0, fd;

	lp_fill(comp, 'd', LP_COMP_LEN);

	if (!mkdtemp(base) || len + 1 > base_cap || len + 1 > abscap)
		return -1;
	memcpy(base_out, base, len + 1);
	if (chdir(base) != 0)
		return -1;
	memcpy(absdir, base, len + 1);

	/* Descend until the directory path alone exceeds PATH_MAX, so the walk
	 * exercises the multi-chunk openat chain (not just one openat). */
	while (len < (size_t)PATH_MAX + 128) {
		if (mkdir(comp, 0700) != 0 || chdir(comp) != 0)
			return -1;
		if (len + 1 + LP_COMP_LEN + 1 > abscap)
			return -1;
		absdir[len++] = '/';
		memcpy(absdir + len, comp, LP_COMP_LEN + 1);
		len += LP_COMP_LEN;
		levels++;
	}

	fd = open(victim, O_CREAT | O_WRONLY | O_TRUNC, 0600);
	if (fd < 0)
		return -1;
	if (write(fd, contents, strlen(contents)) != (ssize_t)strlen(contents)) {
		close(fd);
		return -1;
	}
	close(fd);

	*out_levels = levels;
	return 0;
}

static void lp_destroy_deep(int savedcwd, const char *base, const char *victim,
			    int levels)
{
	char comp[LP_COMP_LEN + 1];
	int i;

	lp_fill(comp, 'd', LP_COMP_LEN);

	/* CWD is the leaf dir: drop the file, then climb + rmdir each level. */
	unlink(victim);
	for (i = 0; i < levels; i++) {
		if (chdir("..") != 0)
			break;
		rmdir(comp);
	}
	if (savedcwd >= 0 && fchdir(savedcwd) != 0)
		return;
	rmdir(base);
}

MU_TEST(test_longpath) {
	const char *contents = "over-the-PATH_MAX limit\n";
	char victim[LP_COMP_LEN + 1];
	char absdir[20000];
	char base[64];
	char leaf[20000];
	char miss_leaf[20000];
	char miss_mid[20000];
	int levels = 0;
	int savedcwd = open(".", O_PATH | O_CLOEXEC);
	struct stat st;
	int fd, bfd, n;
	char buf[128] = { 0 };
	ssize_t r;
	DIR *d;
	struct dirent *de;
	bool found = false;

	lp_fill(victim, 'v', LP_COMP_LEN);

	mu_check(savedcwd >= 0);
	mu_check(lp_make_deep(absdir, sizeof(absdir), base, sizeof(base),
			      &levels, victim, contents) == 0);
	/* The directory itself is past PATH_MAX, forcing the chunked walk. */
	mu_check(strlen(absdir) > PATH_MAX);

	n = snprintf(leaf, sizeof(leaf), "%s/%s", absdir, victim);
	mu_check(n > 0 && (size_t)n < sizeof(leaf));
	mu_check(strlen(leaf) > PATH_MAX);

	/* 1. open + read the deep file. */
	fd = longpath_open(leaf, O_RDONLY);
	mu_check(fd >= 0);
	r = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	mu_check(r == (ssize_t)strlen(contents));
	mu_check(strcmp(buf, contents) == 0);

	/* 2. stat + lstat the deep file. */
	mu_check(longpath_stat(leaf, &st) == 0);
	mu_check((size_t)st.st_size == strlen(contents));
	memset(&st, 0, sizeof(st));
	mu_check(longpath_lstat(leaf, &st) == 0);
	mu_check((size_t)st.st_size == strlen(contents));

	/* 3. opendir the deep directory and list it. */
	d = longpath_opendir(absdir);
	mu_check(d != NULL);
	while ((de = readdir(d))) {
		if (strcmp(de->d_name, victim) == 0) {
			found = true;
			break;
		}
	}
	closedir(d);
	mu_check(found);

	/* 4a. missing final component → ENOENT, not ENAMETOOLONG. */
	{
		char missname[LP_COMP_LEN + 1];

		lp_fill(missname, 'x', LP_COMP_LEN);
		snprintf(miss_leaf, sizeof(miss_leaf), "%s/%s", absdir, missname);
		mu_check(strlen(miss_leaf) > PATH_MAX);
		errno = 0;
		mu_check(longpath_open(miss_leaf, O_RDONLY) < 0);
		mu_check(errno == ENOENT);
		errno = 0;
		mu_check(longpath_stat(miss_leaf, &st) < 0);
		mu_check(errno == ENOENT);
	}

	/* 4b. missing intermediate directory → ENOENT from the ancestor walk. */
	{
		char missdir[LP_COMP_LEN + 1];

		lp_fill(missdir, 'z', LP_COMP_LEN);
		snprintf(miss_mid, sizeof(miss_mid), "%s/%s/%s", absdir,
			 missdir, victim);
		mu_check(strlen(miss_mid) > PATH_MAX);
		errno = 0;
		mu_check(longpath_open(miss_mid, O_RDONLY) < 0);
		mu_check(errno == ENOENT);
	}

	/* 5. short path: identical to plain open(). */
	bfd = longpath_open(base, O_RDONLY | O_DIRECTORY);
	mu_check(bfd >= 0);
	close(bfd);

	lp_destroy_deep(savedcwd, base, victim, levels);
	close(savedcwd);
}

MU_TEST_SUITE(test_suite) {
	MU_RUN_TEST(test_is_block_zeroed);
	MU_RUN_TEST(test_block_len);
	MU_RUN_TEST(test_is_file_renamed);
	MU_RUN_TEST(test_seen_inode);
	MU_RUN_TEST(test_get_extent);
	MU_RUN_TEST(test_sanitize_ctrl);
	MU_RUN_TEST(test_storage_recommend_io_threads);
	MU_RUN_TEST(test_scan_bucket);
	MU_RUN_TEST(test_scan_workq_priority);
	MU_RUN_TEST(test_scan_eta);
	MU_RUN_TEST(test_group_u64);
	MU_RUN_TEST(test_longpath);
}

int main(int argc [[maybe_unused]], char *argv[]) {
	exec_path = argv[0];
	MU_RUN_SUITE(test_suite);
	MU_REPORT();
	return MU_EXIT_CODE;
}
