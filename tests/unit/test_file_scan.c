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
 * Run process_extents() over one buffer holding [0, len) of a file laid out
 * as `recs`, with the file's bytes in `buf` (holes as zeroes, as a read gives
 * them). Leaves what it stored in `h`; the caller frees h->extents.
 */
static void extents_of(const struct fm_rec *recs, unsigned int n,
		       char *buf, size_t len, struct hashes *h,
		       bool *csum_left)
{
	struct buffer b = { .buf = buf, .size = len, .dl_len = len };
	struct scan_ctxt ctxt = { .fd = -1, .filesize = len,
				  .fiemap = mkmap(recs, n) };

	if (process_extents(&ctxt, &b, h, len))
		abort();
	*csum_left = ctxt.extent_csum != NULL;
	if (ctxt.extent_csum)
		finish_running_checksum(ctxt.extent_csum, NULL);
	free(ctxt.fiemap);
}

/*
 * A hole that does not fill whole hash blocks is read, and comes back as
 * zeroes. They are no part of the extent after it, whose row says it starts
 * at fe_logical: hashed in, the same extent behind holes of different sizes
 * got a different digest each time and the extent pass never matched it.
 */
MU_TEST(test_an_extent_digest_leaves_out_the_hole_before_it) {
	const uint64_t K = 1024;
	/* hole [0, 64K), data [64K, 128K) */
	const struct fm_rec lead[] = { { 64 * K, 1024 * K, 64 * K,
					 FIEMAP_EXTENT_LAST } };
	/* data [0, 16K), hole, data [48K, 128K) */
	const struct fm_rec mid[] = { { 0, 1024 * K, 16 * K, 0 },
				      { 48 * K, 2048 * K, 80 * K,
					FIEMAP_EXTENT_LAST } };
	/* data [0, 16K), hole past the buffer, data [1M, 1M + 4K) */
	const struct fm_rec far[] = { { 0, 1024 * K, 16 * K, 0 },
				      { 1024 * K, 2048 * K, 4 * K,
					FIEMAP_EXTENT_LAST } };
	static char buf[128 * 1024];
	unsigned char want[DIGEST_LEN];
	struct hashes h = {0,};
	bool left;

	for (size_t i = 0; i < sizeof(buf); i++)
		buf[i] = (char)(i * 7 + 3);

	memset(buf, 0, 64 * K);
	extents_of(lead, 1, buf, sizeof(buf), &h, &left);
	mu_check(h.extents_index == 1);
	mu_check(h.extents[0].loff == 64 * K && h.extents[0].len == 64 * K);
	checksum_block(buf + 64 * K, 64 * K, want);
	mu_check(memcmp(h.extents[0].digest, want, DIGEST_LEN) == 0);
	free(h.extents);

	for (size_t i = 0; i < sizeof(buf); i++)
		buf[i] = (char)(i * 7 + 3);
	memset(buf + 16 * K, 0, 32 * K);
	h = (struct hashes){0,};
	extents_of(mid, 2, buf, sizeof(buf), &h, &left);
	mu_check(h.extents_index == 2);
	checksum_block(buf, 16 * K, want);
	mu_check(memcmp(h.extents[0].digest, want, DIGEST_LEN) == 0);
	mu_check(h.extents[1].loff == 48 * K && h.extents[1].len == 80 * K);
	checksum_block(buf + 48 * K, 80 * K, want);
	mu_check(memcmp(h.extents[1].digest, want, DIGEST_LEN) == 0);
	free(h.extents);

	/*
	 * The buffer ends in the hole: nothing of the next extent was read, so
	 * there is no digest in progress for a checkpoint to carry (#159).
	 */
	memset(buf + 16 * K, 0, sizeof(buf) - 16 * K);
	h = (struct hashes){0,};
	extents_of(far, 2, buf, sizeof(buf), &h, &left);
	mu_check(h.extents_index == 1);
	mu_check(!left);
	free(h.extents);
}

/*
 * Which gaps a binary older than FILE_HOLE_EXTENT_CHECKED read into the next
 * extent's digest. A row is rehashed for it, and an extent state such a binary
 * checkpointed after one is refused, so a "no" here is a digest kept for good.
 */
MU_TEST(test_hole_read_before) {
	/* Set, not read: test_is_block_zeroed() leaves it at 100. */
	const uint64_t B = DEFAULT_BLOCKSIZE, K = 1024;
	unsigned int saved = blocksize;
	const uint32_t L = FIEMAP_EXTENT_LAST;
	/* Contiguous from 0, and a whole-block hole after whole-block data. */
	const struct fm_rec clean[] = { { 0, 1 * B, B, 0 },
					{ B, 2 * B, B, 0 },
					{ 3 * B, 4 * B, B, L } };
	/* A whole-block hole at the start of the file: skipped, too. */
	const struct fm_rec lead_aligned[] = { { B, 1 * B, B, L } };
	/* A hole at the start that ends mid-block. */
	const struct fm_rec lead_unaligned[] = { { 64 * K, 1 * B, B, L } };
	/* Data that ends mid-block, then a hole up to a block boundary. */
	const struct fm_rec after_short[] = { { 0, 1 * B, 16 * K, 0 },
					      { B, 2 * B, B, L } };
	/* Whole-block data, then a hole that ends mid-block. */
	const struct fm_rec into_block[] = { { 0, 1 * B, B, 0 },
					     { 2 * B + 4 * K, 2 * B, B, L } };
	/* A whole-block hole after a preallocated extent. */
	const struct fm_rec after_prealloc[] = {
		{ 0, 1 * B, B, FIEMAP_EXTENT_UNWRITTEN },
		{ 2 * B, 2 * B, B, L } };
	/* Data that ends mid-block, but no gap after it. */
	const struct fm_rec no_gap[] = { { 0, 1 * B, 16 * K, 0 },
					 { 16 * K, 2 * B, B, L } };
	struct fiemap *fm;

	blocksize = B;
	fm = mkmap(clean, 3);
	mu_check(!hole_read_before(fm, 0));
	mu_check(!hole_read_before(fm, 1));
	mu_check(!hole_read_before(fm, 2));
	free(fm);

	fm = mkmap(lead_aligned, 1);
	mu_check(!hole_read_before(fm, 0));
	free(fm);

	fm = mkmap(lead_unaligned, 1);
	mu_check(hole_read_before(fm, 0));
	free(fm);

	fm = mkmap(after_short, 2);
	mu_check(hole_read_before(fm, 1));
	free(fm);

	fm = mkmap(into_block, 2);
	mu_check(hole_read_before(fm, 1));
	free(fm);

	fm = mkmap(after_prealloc, 2);
	mu_check(hole_read_before(fm, 1));
	free(fm);

	fm = mkmap(no_gap, 2);
	mu_check(!hole_read_before(fm, 1));
	free(fm);
	blocksize = saved;
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

static void probe_alarm(int sig) { (void)sig; }

/*
 * The FIDEDUPERANGE probe opens a file the walk listed as regular, and a FIFO
 * may sit at that name by now. A 0444 one fails the read-write open with
 * EACCES, and a plain read-only fallback then waits for a writer, holding up
 * the whole walk. It must neither block nor be counted: it says nothing about
 * the filesystem. An alarm without SA_RESTART turns a blocked open into
 * EINTR, so a regression fails here instead of hanging the suite.
 */
MU_TEST(test_the_probe_skips_a_fifo_without_blocking) {
	struct statx st = { .stx_mode = S_IFREG | 0444, .stx_size = 1 << 20,
			    .stx_uid = geteuid() };
	struct sigaction sa = { .sa_handler = probe_alarm }, old;
	unsigned int tries = locked_fs.dedupe_probe_tries;
	unsigned int unasked = locked_fs.dedupe_probe_unasked;
	enum dedupe_support dedupe = locked_fs.dedupe;
	char path[PATH_MAX];
	bool settled;

	if ((size_t)snprintf(path, sizeof(path), "%s/oans-probe-fifo-%d",
			     test_scratch_dir(), (int)getpid()) >= sizeof(path))
		abort();
	unlink(path);
	if (mkfifo(path, 0444))
		abort();

	sigemptyset(&sa.sa_mask);
	sigaction(SIGALRM, &sa, &old);
	alarm(2);
	settled = fs_dedupe_probe_settled(path, &st);
	alarm(0);
	sigaction(SIGALRM, &old, NULL);
	unlink(path);

	mu_check(settled);
	mu_check(locked_fs.dedupe_probe_tries == tries);
	mu_check(locked_fs.dedupe_probe_unasked == unasked);
	mu_check(locked_fs.dedupe == dedupe);
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
