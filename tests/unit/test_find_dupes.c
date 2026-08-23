/*
 * The extent search that --dedupe-options=partial runs.
 *
 * Its own translation unit. The sources below are #included rather than
 * linked, because tests here call their static functions; every other source
 * the suite needs is compiled once and linked, which is what makes a mutant
 * rebuild one subject instead of all of them.
 */
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

#define FD_BLOCK	4096

/*
 * `blocksize` is a mutable global that block_len() reads, and an earlier test
 * leaves it at 100. Each of these sets it and puts it back, so they neither
 * depend on test order nor impose one.
 *
 * It is the only one that needs saving. The reachable code also reads `debug`,
 * which no test in this file assigns - a future one that does should save it
 * here too. `options.dedupe_same_file` is *not* read: only search_extent()
 * consults it, and these call compare_extents() directly.
 */
struct fd_fixture {
	struct hash_tree tree;
	struct results_tree res;
	unsigned int saved_blocksize;
};

static void fd_begin(struct fd_fixture *f)
{
	free_all_filerecs();
	init_hash_tree(&f->tree);
	init_results_tree(&f->res);
	f->saved_blocksize = blocksize;
	blocksize = FD_BLOCK;
}

static void fd_end(struct fd_fixture *f)
{
	free_results_tree(&f->res);
	free_hash_tree(&f->tree);
	free_all_filerecs();
	blocksize = f->saved_blocksize;
}

/* Blocks at the offsets given, so a file can have a hole where nothing was
 * hashed. The file ends `tail` bytes into its last block, or on a block
 * boundary when `tail` is zero. */
static struct filerec *fd_file_at(struct fd_fixture *f, int64_t id,
				  const unsigned int *digests,
				  const uint64_t *offs, unsigned int n,
				  uint64_t tail)
{
	char name[64];
	struct filerec *file;

	snprintf(name, sizeof(name), "/tree/f%lld", (long long)id);
	file = filerec_new(name, id, offs[n - 1] + (tail ? tail : FD_BLOCK));
	if (!file)
		abort();
	for (unsigned int i = 0; i < n; i++) {
		unsigned char dg[DIGEST_LEN];

		digest_of(dg, digests[i]);
		if (insert_hashed_block(&f->tree, dg, file, offs[i]))
			abort();
	}
	return file;
}

/* The same, with the blocks packed contiguously from zero. */
static struct filerec *fd_file(struct fd_fixture *f, int64_t id,
			       const unsigned int *digests, unsigned int n,
			       uint64_t tail)
{
	uint64_t offs[8];

	if (n > ARRAY_SIZE(offs))
		abort();
	for (unsigned int i = 0; i < n; i++)
		offs[i] = FD_BLOCK * i;
	return fd_file_at(f, id, digests, offs, n, tail);
}

/* A block of a file by offset; aborts if there is none, which would mean the
 * fixture is broken rather than the code under test. */
static struct file_block *fd_block(struct filerec *file, uint64_t loff)
{
	struct file_block *b = find_filerec_block(file, loff);

	if (!b)
		abort();
	return b;
}

/*
 * Two files whose blocks all match produce one group covering the whole run.
 *
 * The length is the part worth stating: record_match() ends the range at
 * `block_len(end) + end->b_loff - 1`, an *inclusive* offset, and
 * insert_result() reads a length back out of it as `endoff - startoff + 1`.
 * Those two have to agree, and an off-by-one either way files the group under
 * a length no other extent is filed under - where it silently meets nothing.
 */
MU_TEST(test_a_whole_matching_run_is_recorded_as_one_group) {
	struct fd_fixture f;
	static const unsigned int dg[] = { 1, 2, 3 };
	struct filerec *a, *b;
	struct dupe_extents *d;

	fd_begin(&f);
	a = fd_file(&f, 1, dg, ARRAY_SIZE(dg), 0);
	b = fd_file(&f, 2, dg, ARRAY_SIZE(dg), 0);

	mu_check(compare_extents(a, fd_block(a, 0), b, fd_block(b, 0),
				 FD_BLOCK * ARRAY_SIZE(dg), &f.res) == 0);

	mu_check(f.res.num_dupes == 1);
	d = only_group(&f.res);
	mu_check(d->de_num_dupes == 2);
	mu_check(d->de_len == FD_BLOCK * ARRAY_SIZE(dg));

	/* Both members start at zero, which is where the run started. */
	{
		struct extent *e;

		list_for_each_entry(e, &d->de_extents, e_list)
			mu_check(e->e_loff == 0);
	}
	fd_end(&f);
}

/*
 * A digest that differs in the middle ends the run there, and the search
 * resumes past it rather than stopping - so two separate runs become two
 * groups, not one long one and not one short one.
 */
MU_TEST(test_a_mismatch_splits_the_run_rather_than_ending_the_search) {
	struct fd_fixture f;
	/* Same, different, same-again: two runs of one block each. */
	static const unsigned int left[]  = { 1, 2, 3 };
	static const unsigned int right[] = { 1, 9, 3 };
	struct filerec *a, *b;
	struct rb_node *n;
	unsigned int groups = 0, seen = 0;

	fd_begin(&f);
	a = fd_file(&f, 1, left, ARRAY_SIZE(left), 0);
	b = fd_file(&f, 2, right, ARRAY_SIZE(right), 0);

	mu_check(compare_extents(a, fd_block(a, 0), b, fd_block(b, 0),
				 FD_BLOCK * ARRAY_SIZE(left), &f.res) == 0);

	/*
	 * Each matching block is its own group of exactly one block, and
	 * crucially neither *starts* at the mismatched block: a group running
	 * 4096..12287 would cover bytes that differ, which is what this found
	 * when it was first written.
	 */
	for (n = rb_first(&f.res.root); n; n = rb_next(n)) {
		struct dupe_extents *d =
			rb_entry(n, struct dupe_extents, de_node);
		struct extent *e;

		mu_check(d->de_len == FD_BLOCK);
		list_for_each_entry(e, &d->de_extents, e_list) {
			mu_check(e->e_loff == 0 || e->e_loff == FD_BLOCK * 2);
			seen |= e->e_loff == 0 ? 1u : 2u;
		}
		groups++;
	}
	mu_check(groups == 2);
	/* One group at each of the two matching offsets - not both at the same
	 * one, which the per-extent check above would allow. */
	mu_check(seen == 3);
	mu_check(f.res.num_extents == 4);	/* two members per group */

	fd_end(&f);
}

/*
 * The two sides are recorded separately, and asymmetric offsets are what makes
 * that observable.
 *
 * record_match() fills recs[], soff[] and eoff[] as pairs, one slot per file.
 * With both files laid out identically - which every other test here does,
 * because it is the natural fixture - slot 0 and slot 1 hold the same numbers,
 * so writing either into both, or reading the wrong one, changes nothing any
 * assertion can see. Putting the matching run at a different offset in each
 * file makes the two slots hold different values, and the pair of recorded
 * offsets then pins which is which.
 */
MU_TEST(test_each_side_of_a_match_records_its_own_offsets) {
	struct fd_fixture f;
	static const unsigned int dg[] = { 1, 2, 3 };
	static const uint64_t early[] = { 0, FD_BLOCK, FD_BLOCK * 2 };
	static const uint64_t late[]  = { FD_BLOCK * 2, FD_BLOCK * 3, FD_BLOCK * 4 };
	struct filerec *a, *b;
	struct dupe_extents *d;
	struct extent *e;
	unsigned int seen = 0;

	fd_begin(&f);
	a = fd_file_at(&f, 1, dg, early, ARRAY_SIZE(dg), 0);
	b = fd_file_at(&f, 2, dg, late, ARRAY_SIZE(dg), 0);

	mu_check(compare_extents(a, fd_block(a, 0), b, fd_block(b, FD_BLOCK * 2),
				 FD_BLOCK * 8, &f.res) == 0);

	mu_check(f.res.num_dupes == 1);
	d = only_group(&f.res);
	mu_check(d->de_num_dupes == 2);
	mu_check(d->de_len == FD_BLOCK * 3);

	/*
	 * a's copy starts at 0 and b's at two blocks in - each from its own
	 * slot. A slot written from the wrong side puts both at one offset.
	 */
	list_for_each_entry(e, &d->de_extents, e_list) {
		if (e->e_file == a) {
			mu_check(e->e_loff == 0);
			seen |= 1u;
		} else if (e->e_file == b) {
			mu_check(e->e_loff == FD_BLOCK * 2);
			seen |= 2u;
		} else {
			mu_fail("an extent belonging to neither file");
		}
	}
	mu_check(seen == 3);	/* one member from each file, not two of one */

	fd_end(&f);
}

/*
 * A run stops where the blocks stop being contiguous, even though they still
 * match.
 *
 * Matching digests are not enough: the recorded range is a span of bytes in
 * both files, so a run that jumps a hole in either one describes bytes that
 * were never compared. The kernel byte-verifies and refuses the request, and
 * the parts that really did match go down with it.
 */
MU_TEST(test_a_run_stops_where_the_blocks_stop_being_contiguous) {
	struct fd_fixture f;
	static const unsigned int dg[] = { 1, 2, 3 };
	static const uint64_t gapless[] = { 0, FD_BLOCK, FD_BLOCK * 2 };
	/* Same three digests, but the third block sits past a hole. */
	static const uint64_t holed[]   = { 0, FD_BLOCK, FD_BLOCK * 3 };
	struct filerec *a, *b;
	struct dupe_extents *d;
	struct extent *e;

	fd_begin(&f);
	a = fd_file_at(&f, 1, dg, gapless, ARRAY_SIZE(dg), 0);
	b = fd_file_at(&f, 2, dg, holed, ARRAY_SIZE(dg), 0);

	mu_check(compare_extents(a, fd_block(a, 0), b, fd_block(b, 0),
				 FD_BLOCK * 4, &f.res) == 0);

	/*
	 * Exactly one group of exactly two blocks, starting at zero. The third
	 * pair matches on digest and is deliberately never recorded: reaching
	 * it would mean a range spanning b's hole, i.e. bytes that were never
	 * compared. Asserting only "not three blocks" would accept every
	 * found-too-short answer as well, which is half of what this is about.
	 */
	mu_check(f.res.num_dupes == 1);
	d = only_group(&f.res);
	mu_check(d->de_num_dupes == 2);
	mu_check(d->de_len == FD_BLOCK * 2);
	mu_check(f.res.num_extents == 2);
	list_for_each_entry(e, &d->de_extents, e_list)
		mu_check(e->e_loff == 0);

	fd_end(&f);
}

/*
 * The last block of a file that is not a whole number of blocks long is
 * shorter, and the recorded length has to say so. Asking the kernel to
 * deduplicate past the end of a file is a request it refuses, so a run
 * measured in whole blocks here is work that fails later with nothing
 * connecting the failure to this decision.
 */
MU_TEST(test_a_short_final_block_shortens_the_recorded_run) {
	struct fd_fixture f;
	static const unsigned int dg[] = { 1, 2 };
	struct filerec *a, *b;
	struct dupe_extents *d;

	fd_begin(&f);
	/* Both files end 1000 bytes into their second block. */
	a = fd_file(&f, 1, dg, ARRAY_SIZE(dg), 1000);
	b = fd_file(&f, 2, dg, ARRAY_SIZE(dg), 1000);
	mu_check(a->size == FD_BLOCK + 1000);

	mu_check(compare_extents(a, fd_block(a, 0), b, fd_block(b, 0),
				 FD_BLOCK * 2, &f.res) == 0);

	mu_check(f.res.num_dupes == 1);
	d = only_group(&f.res);
	/* One whole block plus the 1000-byte tail, not two whole blocks. */
	mu_check(d->de_len == FD_BLOCK + 1000);

	fd_end(&f);
}

/*
 * The search driven the way the dedupe phase drives it.
 *
 * This is `--dedupe-options=partial`: a thread pool over the global filerec
 * list, each worker asking the hashfile which of its file's extents nothing
 * else shares, then searching those block by block for matches the extent pass
 * could not see.
 *
 * **One test, several scenarios, and that is not laziness.** The pool must be
 * created once per process, because search_file_extents() caches its database
 * handle in a `static __thread` and GLib caches idle pool threads: a second
 * extents_search_init() hands work to the same OS threads, whose thread-local
 * still points at the handle the first free_pool() released, and whose
 * `if (!db)` is therefore false. ASAN calls that a heap-use-after-free, and it
 * is - but only for a caller that builds the pool twice. Production builds it
 * once around the whole dedupe phase (oans.c) and calls the search once per
 * generation window inside, so the handle lives exactly as long as the pool
 * that owns it. Splitting these into four MU_TESTs would test a lifetime oans
 * does not have.
 */
struct search_fixture {
	struct fd_fixture fd;
	unsigned int saved_cpu_threads;
	int saved_quiet;
	bool saved_same_file;
};

/*
 * Teardown by scope exit, not at the end of the body: mu_check() *returns* on
 * failure, so a failed assertion would otherwise leave the pool alive with its
 * per-thread handles open - and those handles are the only thing keeping the
 * shared in-memory database alive, so its rows would survive into every later
 * test. One failing assertion cost eight unrelated dbfile tests before this
 * was a cleanup attribute.
 */
static void search_end(struct search_fixture *s)
{
	extents_search_free();
	search_delay_us = 0;
	options.cpu_threads = s->saved_cpu_threads;
	options.dedupe_same_file = s->saved_same_file;
	quiet = s->saved_quiet;
	fd_end(&s->fd);
}

static void search_begin(struct search_fixture *s, struct dbhandle *db)
{
	struct dbfile_config cfg = {0};

	fd_begin(&s->fd);			/* sets the global blocksize */
	s->saved_cpu_threads = options.cpu_threads;
	s->saved_quiet = quiet;
	s->saved_same_file = options.dedupe_same_file;
	options.cpu_threads = 2;
	options.dedupe_same_file = false;
	/* Silences find_additional_dedupe()'s banner. The progress bar it also
	 * starts cannot be: psearch_progress_thread() printf()s unguarded. */
	quiet = 1;

	/*
	 * The stored config has to agree with the blocksize just set. Every
	 * dbfile_open_handle() runs dbfile_check(), which treats the hashfile's
	 * blocksize as authoritative and writes it back over the global - so a
	 * worker opening its own handle would otherwise undo fd_begin()
	 * mid-test, and the runs come back measured in the wrong unit.
	 */
	cfg.blocksize = blocksize;
	memcpy(cfg.hash_type, HASH_TYPE, 8);
	cfg.major = DB_FILE_MAJOR;
	cfg.minor = DB_FILE_MINOR;
	if (dbfile_sync_config(db, &cfg))
		abort();

	extents_search_init();
}

/* Between scenarios: the trees and the filerec registry go, and so do the rows,
 * since every memdb() handle is the same shared in-memory database. */
static void search_reset(struct search_fixture *s, struct dbhandle *db)
{
	free_results_tree(&s->fd.res);
	free_hash_tree(&s->fd.tree);
	free_all_filerecs();
	exec(db, "delete from files");		/* hashes cascade */
	init_hash_tree(&s->fd.tree);
	init_results_tree(&s->fd.res);
}

/*
 * A file with `n` blocks, plus one extent row whose digest nothing else shares
 * - which is what makes the search consider the file at all.
 *
 * The row's name and the filerec's differ, deliberately and harmlessly: only
 * the row is looked up by name, and fd_file() names the in-memory file after
 * its id. poff is zero because nothing under test reads it - search_extent()
 * takes only loff and len, and the rest reaches a dprintf.
 */
static void search_file(struct search_fixture *s, struct dbhandle *db,
			const char *name, uint64_t ino, unsigned int extent_dg,
			const unsigned int *blocks, unsigned int n)
{
	struct extent_csum ext[1];
	int64_t id = put_dupe(db, name, ino, extent_dg, FD_BLOCK * n, 1, 0, 1);

	ext[0].loff = 0;
	ext[0].poff = 0;
	ext[0].len = FD_BLOCK * n;
	digest_of(ext[0].digest, extent_dg);
	if (dbfile_store_extent_hashes(db, id, 1, ext))
		abort();

	if (!fd_file(&s->fd, id, blocks, n, 0))
		abort();
}

/*
 * ---------------------------------------------------------------------------
 * The interrupt flag (#201)
 *
 * SIGINT and SIGTERM set a flag; every loop that owns state notices it and
 * unwinds through its ordinary exit, so the batched writer commits on the way
 * out. What fails silently here is the whole of it: a flag that is never set
 * makes a Ctrl-C look ignored for minutes, and a second one that kills before
 * the flush throws away exactly the work the feature exists to keep.
 *
 * One test with ordered scenarios, because the state is global *and* one-shot:
 * SA_RESETHAND puts the default action back on delivery, so a raise with no
 * handler installed would kill the suite rather than fail it. Every scenario
 * below re-installs before it raises, and puts the flag back afterwards.
 * tests.c #includes interrupt.c, so the statics are reachable to do that -
 * without it none of this would be testable at all.
 * ---------------------------------------------------------------------------
 */

/* What the kernel currently has installed for a signal. */

MU_TEST(test_the_extent_search_driven_by_its_pool) {
	_cleanup_(sqlite3_close_cleanup) struct dbhandle *db = memdb();
	_cleanup_(search_end) struct search_fixture s = {0};
	static const unsigned int blocks[] = { 11, 12, 13 };
	struct dupe_extents *d;

	search_begin(&s, db);

	/*
	 * A match the extent pass could not see: two extents that hash
	 * differently, holding blocks that agree. If this ever finds nothing,
	 * partial mode has quietly become a no-op - which would otherwise show
	 * up only as "we seem to reclaim less than we used to".
	 */
	search_file(&s, db, "/tree/a", 1, 41, blocks, ARRAY_SIZE(blocks));
	search_file(&s, db, "/tree/b", 2, 42, blocks, ARRAY_SIZE(blocks));

	/*
	 * Hold the workers back for this one call, so the idleness assertion
	 * below is a claim rather than a race the fixture wins by default.
	 * It has to exceed psearch_join()'s latency, not just the workers':
	 * with the wait deleted, find_additional_dedupe() still blocks in
	 * psearch_join() -> printer_stop() for the remainder of the progress
	 * thread's 100ms sleep, which masks any shorter delay. Measured: at
	 * 2ms the deletion survives, at 300ms it is caught.
	 *
	 * Set directly rather than via DUPEREMOVE_SEARCH_DELAY_MS because
	 * tests.c #includes find_dupes.c and extents_search_init() reads the
	 * environment. Scoped to this call alone - paying it on all four costs
	 * a second of suite time, and suite runtime feeds the mutation tool's
	 * hang timeout.
	 */
	search_delay_us = 300000;
	mu_check(find_additional_dedupe(&s.fd.res) == 0);
	search_delay_us = 0;
	mu_assert(s.fd.res.num_dupes == 1, "a block-level match was not found");
	d = only_group(&s.fd.res);
	mu_check(d->de_num_dupes == 2);
	mu_check(d->de_len == FD_BLOCK * ARRAY_SIZE(blocks));

	/*
	 * The search is idle by the time it returns, which is #123 rather than
	 * tidiness: each worker holds a filerec from the global list, and the
	 * caller goes straight back to reaping batches - which frees them.
	 * Returning with work in flight lets a worker read a freed filerec.
	 * That reproduced in about one memcheck run in twenty.
	 */
	mu_assert(extents_search_idle(), "the search returned with work still running");

	/* A file of no bytes is skipped rather than handed to the pool, and the
	 * two real files still find each other. */
	search_reset(&s, db);
	search_file(&s, db, "/tree/c", 3, 43, blocks, ARRAY_SIZE(blocks));
	search_file(&s, db, "/tree/d", 4, 44, blocks, ARRAY_SIZE(blocks));
	if (!filerec_new("/tree/empty", 99, 0))
		abort();

	mu_check(find_additional_dedupe(&s.fd.res) == 0);
	mu_assert(s.fd.res.num_dupes == 1, "an empty file disturbed the search");
	mu_check(extents_search_idle());
	/*
	 * The results tree cannot see the skip at all: a worker handed the
	 * empty file would find no extent rows for it and return without
	 * touching the tree, so `num_dupes == 1` holds either way. What the
	 * branch uniquely does is count the file as processed itself, so that
	 * is what is asserted - three files, all accounted for. Deleting the
	 * count gives two; deleting the `continue` gives four, because the
	 * worker counts it as well.
	 */
	mu_assert(search_processed == 3,
		  "an empty file was not counted as processed exactly once");

	/*
	 * Two halves of one file are not matched against each other unless
	 * asked for. oans will deduplicate a file against itself on request and
	 * must not by default - the win is usually nil and the surprise is not.
	 */
	search_reset(&s, db);
	{
		static const unsigned int twice[] = { 61, 62, 61, 62 };

		search_file(&s, db, "/tree/self", 5, 45, twice, ARRAY_SIZE(twice));
		mu_check(find_additional_dedupe(&s.fd.res) == 0);
		mu_assert(s.fd.res.num_dupes == 0,
			  "a file was matched against itself by default");

		/* ...and does when it is asked. */
		options.dedupe_same_file = true;
		mu_check(find_additional_dedupe(&s.fd.res) == 0);
		mu_assert(s.fd.res.num_dupes == 1,
			  "dedupe_same_file did not enable a self match");
		/* And it is the right group: blocks 0-1 matched against 2-3.
		 * Checking only that *a* group exists would survive a mutant
		 * that recorded the wrong range - which is the bug this file
		 * was written around. */
		d = only_group(&s.fd.res);
		mu_check(d->de_num_dupes == 2);
		mu_check(d->de_len == FD_BLOCK * 2);
	}
}
