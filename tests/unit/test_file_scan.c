/*
 * The walk: zero blocks, rename detection, the inode set and the queue.
 *
 * Compiled as part of tu_scan.c, which is where the sources these tests reach
 * into are #included.
 */
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

/*
 * A read buffer may be faked with zeroes only when no byte of it is data
 * (#273). Answering true for any buffer that merely reached a preallocated
 * extent gave two different files one digest.
 */
MU_TEST(test_is_area_ignored) {
	const uint32_t U = FIEMAP_EXTENT_UNWRITTEN, L = FIEMAP_EXTENT_LAST;
	const uint64_t K = 1024, M = 1024 * K;
	/* data [0, 512K), preallocated [512K, 1M) */
	const struct fm_rec tail[] = { { 0, 1 * M, 512 * K, 0 },
				       { 512 * K, 2 * M, 512 * K, U | L } };
	/* data [0, 64K), hole, preallocated [8M, 9M) */
	const struct fm_rec gap[] = { { 0, 1 * M, 64 * K, 0 },
				      { 8 * M, 2 * M, M, U | L } };
	/* preallocated [0, 4K), hole, preallocated [8K, 12K), data [12K, 16K) */
	const struct fm_rec mixed[] = { { 0, 1 * M, 4 * K, U },
					{ 8 * K, 2 * M, 4 * K, U },
					{ 12 * K, 3 * M, 4 * K, L } };
	struct fiemap *fm = mkmap(tail, 2);
	unsigned int cur = 0;

	mu_check(!is_area_ignored(fm, 0, M, NULL));	/* data, then prealloc */
	mu_check(is_area_ignored(fm, 512 * K, 512 * K, NULL));
	mu_check(!is_area_ignored(fm, 508 * K, 4 * K, NULL));	/* last data block */
	mu_check(is_area_ignored(fm, 512 * K, 4 * K, NULL));

	/* The hint is left at the extent the area starts in, not further on. */
	mu_check(!is_area_ignored(fm, 0, M, &cur));
	mu_check(cur == 0);
	mu_check(is_area_ignored(fm, 512 * K, 512 * K, &cur));
	mu_check(cur == 1);
	free(fm);

	/* On a hole, the next extent must start inside the area to count. */
	fm = mkmap(gap, 2);
	mu_check(!is_area_ignored(fm, 0, M, NULL));
	mu_check(!is_area_ignored(fm, 64 * K, M, NULL));
	mu_check(is_area_ignored(fm, 8 * M - 4 * K, 8 * K, NULL));
	free(fm);

	/* Holes between preallocated extents are fine; data anywhere is not. */
	fm = mkmap(mixed, 3);
	mu_check(is_area_ignored(fm, 0, 12 * K, NULL));
	mu_check(!is_area_ignored(fm, 0, 16 * K, NULL));
	mu_check(!is_area_ignored(fm, 8 * K, 8 * K, NULL));
	free(fm);
}

/*
 * A file the walk queued can be something else by the time the consumer gets
 * it. That used to abort the run, losing the open write batch (#278); it is
 * skipped and counted, before anything touches the hashfile.
 */
MU_TEST(test_a_file_that_became_a_directory_is_skipped) {
	struct statx st = { .stx_mode = S_IFDIR | 0755 };
	uint64_t before[SCAN_SKIP__COUNT], after[SCAN_SKIP__COUNT];

	filescan_get_skips(before);
	mu_check(__scan_file("/nowhere", NULL, &st) == 0);
	filescan_get_skips(after);
	mu_check(after[SCAN_SKIP_NOT_REGULAR] ==
		 before[SCAN_SKIP_NOT_REGULAR] + 1);
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

/*
 * Every bounded writer here has the same contract, so it is stated once: the
 * result is a NUL-terminated prefix of the untruncated form, complete whenever
 * it fits, and nothing outside the caller's size is touched. The fences are the
 * half a length assertion cannot make - `strlen(out) < sz` says nothing about
 * the byte after it, and these are the only callers that can run out of room.
 */
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
