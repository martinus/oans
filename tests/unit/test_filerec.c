/*
 * The filerec registry and its descriptor refcount.
 *
 * Compiled as part of tu_plain.c, which #includes no source at all: nothing
 * here reaches a static, so it links against them like any consumer.
 */

MU_TEST(test_filerec_the_registry_finds_what_was_put_in_it) {
	struct filerec *a, *b;

	/* One global registry serves every test, so each starts it over. */
	free_all_filerecs();
	a = mkfilerec(1);
	b = mkfilerec(2);

	mu_check(filerec_find(1) == a);
	mu_check(filerec_find(2) == b);
	mu_check(filerec_find(99) == NULL);	/* never inserted */

	mu_check(!strcmp(a->filename, "/tree/f1"));
	mu_check(a->size == PROP_LEN);
	mu_check(a->fd == -1);			/* not opened yet */

	free_all_filerecs();
	mu_check(filerec_find(1) == NULL);	/* and teardown really clears */
}

/*
 * The reference count balances, and the free that ends it takes the filerec
 * out of the lookup tree - so a later filerec_find() cannot hand back a
 * pointer to freed memory.
 *
 * Deliberately *not* claiming this pins the cross-batch lifetime rule from
 * CLAUDE.md. That rule is about run_dedupe.c holding a ref per batch, and it
 * is not even true of this module in isolation: free_all_filerecs() frees
 * every filerec whatever its refs, which is what the teardown on the last
 * line of this test does. What is testable here is the smaller claim above.
 */
MU_TEST(test_filerec_is_unfindable_once_its_last_reference_goes) {
	struct filerec *f;

	free_all_filerecs();
	f = mkfilerec(1);
	mu_check(f->refs == 0);		/* created unreferenced */

	filerec_get(f);
	filerec_get(f);
	mu_check(f->refs == 2);

	filerec_put(f);
	mu_check(f->refs == 1);
	mu_check(filerec_find(1) == f);	/* one holder left: still live */

	filerec_put(f);
	mu_check(filerec_find(1) == NULL);

	free_all_filerecs();
}

/*
 * Every filerec is findable by its own id and by no other, for any insertion
 * order. The registry is an rbtree keyed on fileid, so this is what makes
 * cmp_filerecs' answer observable: a comparator that inverts, or that reads
 * the wrong field, still builds *a* tree - just one that cannot find things
 * again.
 */
MU_TEST(test_prop_every_filerec_is_findable_by_its_own_id) {
	declare_prop(p, 200);

	while (prop_next(&p)) {
		uint64_t ids[PROP_BLOCKS];
		struct filerec *made[PROP_BLOCKS];
		unsigned int n = (unsigned int)prop_range(&p, 1, ARRAY_SIZE(ids));
		uint64_t id = 0;

		free_all_filerecs();

		/* Ascending with gaps makes them distinct by construction; the
		 * shuffle is what makes the insertion order arbitrary. The gaps
		 * double as ids to look up and miss on. */
		for (unsigned int i = 0; i < n; i++) {
			id += prop_range(&p, 1, 8);
			ids[i] = id;
		}
		prop_shuffle_u64(&p, ids, n);

		for (unsigned int i = 0; i < n; i++)
			made[i] = mkfilerec((int64_t)ids[i]);

		for (unsigned int i = 0; i < n; i++) {
			prop_check(&p, filerec_find((int64_t)ids[i]) == made[i]);
			prop_check(&p, made[i]->fileid == (int64_t)ids[i]);
		}

		/* Past every id inserted: not answered with a neighbour. */
		for (uint64_t probe = id + 1; probe <= id + 4; probe++)
			prop_check(&p, filerec_find((int64_t)probe) == NULL);
	}
	free_all_filerecs();
}

/*
 * Everything the hash tree counts stays consistent with what is in it, for any
 * mix of files, digests and offsets.
 *
 * Three counters must agree with three different things: tree->num_blocks with
 * the blocks inserted, tree->num_hashes with the *distinct* digests, and each
 * dl_num_elem with that digest's share. Nothing downstream validates them;
 * find_dupes reads them to decide what to compare, so a count that drifts
 * makes oans quietly compare the wrong set.
 *
 * The shadow model (per_digest, next_loff) is kept independently of the tree
 * rather than read back out of it, which is what stops this being a
 * restatement of the code under test.
 */
static struct filerec *mkrealfile(char *path, size_t sz)
{
	int fd;

	snprintf(path, sz, "/tmp/oans-filerec-XXXXXX");
	fd = mkstemp(path);
	if (fd < 0 || write(fd, "hello", 5) != 5)
		abort();
	close(fd);
	return filerec_new(path, 1, 5);
}

/*
 * The file descriptor is reference counted, and that is the whole point: the
 * dedupe phase opens the same file from several groups at once, so a nested
 * open must hand back the descriptor already open rather than a second one.
 * Opening twice and closing twice looks correct either way from the outside -
 * what separates a real refcount from open-and-close-every-time is that the
 * descriptor stays *usable* across the inner close, which is why this reads a
 * byte through it rather than only inspecting the counter.
 */
MU_TEST(test_filerec_holds_one_descriptor_however_often_it_is_opened) {
	char path[64];
	struct filerec *f;
	char buf[1];
	int first_fd;

	free_all_filerecs();
	f = mkrealfile(path, sizeof(path));
	mu_check(f->fd == -1 && f->fd_refs == 0);

	mu_check(filerec_open(f, false) == 0);
	mu_check(f->fd != -1 && f->fd_refs == 1);
	first_fd = f->fd;

	/* Nested open: the same descriptor, not a second one. */
	mu_check(filerec_open(f, false) == 0);
	mu_check(f->fd == first_fd);
	mu_check(f->fd_refs == 2);

	filerec_close(f);
	mu_check(f->fd_refs == 1);
	mu_check(f->fd == first_fd);
	/* Still open, which a close-every-time implementation would fail. */
	mu_check(pread(f->fd, buf, 1, 0) == 1 && buf[0] == 'h');

	filerec_close(f);
	mu_check(f->fd_refs == 0 && f->fd == -1);

	/* A file that is not there reports the errno and leaves the filerec
	 * closed rather than counting a descriptor it never got. */
	unlink(path);
	mu_check(filerec_open(f, true) == ENOENT);
	mu_check(f->fd == -1 && f->fd_refs == 0);

	free_all_filerecs();
}

/*
 * filerec_open_once() is what the dedupe phase uses to open every member of a
 * group without opening any of them twice. It remembers what it has opened in
 * a token tree keyed on the filerec, so asking again is a no-op - and the test
 * of that is the *refcount*, since a second real open would leave a
 * descriptor no close in the batch will ever release.
 */
MU_TEST(test_filerec_open_once_opens_each_file_exactly_once) {
	char path[64];
	struct filerec *f;
	OPEN_ONCE(open_files);

	free_all_filerecs();
	f = mkrealfile(path, sizeof(path));

	mu_check(filerec_open_once(f, &open_files) == 0);
	mu_check(f->fd_refs == 1);

	/* Asked twice, opened once. */
	mu_check(filerec_open_once(f, &open_files) == 0);
	mu_check(f->fd_refs == 1);
	mu_check(filerec_open_once(f, &open_files) == 0);
	mu_check(f->fd_refs == 1);

	filerec_close_open_list(&open_files);
	mu_check(f->fd_refs == 0 && f->fd == -1);
	mu_check(RB_EMPTY_ROOT(&open_files.root));

	/* A missing file is reported, and leaves no token behind claiming it
	 * was opened - otherwise a later pass would skip opening it. */
	unlink(path);
	mu_check(filerec_open_once(f, &open_files) == ENOENT);
	mu_check(RB_EMPTY_ROOT(&open_files.root));
	mu_check(f->fd_refs == 0);

	free_all_filerecs();
}

/*
 * The token tree is keyed on the filerec *pointer*, which is what makes the
 * lookup above cheap. Worth its own case because pointer ordering is the one
 * comparator here whose operands a test cannot choose: the property below
 * inserts whatever addresses the allocator hands out, so it fails only if the
 * comparator disagrees with itself.
 */
MU_TEST(test_prop_a_filerec_token_is_found_by_its_filerec) {
	declare_prop(p, 120);
	struct filerec *files[PROP_FILES];

	free_all_filerecs();
	for (unsigned int i = 0; i < PROP_FILES; i++)
		files[i] = mkfilerec((int64_t)i + 1);

	while (prop_next(&p)) {
		struct rb_root root = RB_ROOT;
		bool inserted[PROP_FILES] = {false};
		uint64_t order[PROP_FILES];
		unsigned int n = (unsigned int)prop_range(&p, 1, PROP_FILES);

		for (unsigned int i = 0; i < PROP_FILES; i++)
			order[i] = i;
		prop_shuffle_u64(&p, order, PROP_FILES);

		for (unsigned int i = 0; i < n; i++) {
			struct filerec *f = files[order[i]];
			struct filerec_token *t = filerec_token_new(f);

			if (!t)
				abort();
			insert_filerec_token_rb(&root, t);
			inserted[order[i]] = true;
		}

		for (unsigned int i = 0; i < PROP_FILES; i++) {
			struct filerec_token *t =
				find_filerec_token_rb(&root, files[i]);

			if (!inserted[i]) {
				prop_check(&p, t == NULL);
				continue;
			}
			prop_check(&p, t != NULL);
			prop_check(&p, t->t_file == files[i]);
		}

		while (!RB_EMPTY_ROOT(&root)) {
			struct filerec_token *t =
				rb_entry(rb_first(&root), struct filerec_token,
					 t_node);

			rb_erase(&t->t_node, &root);
			filerec_token_free(t);
		}
	}
	free_all_filerecs();
}

/*
 * ---------------------------------------------------------------------------
 * The dedupe-phase loaders
 *
 * Where the hashfile meets the in-memory model: five functions turning rows
 * into filerecs, hash trees and results trees. They were 155 mutants with a 0%
 * kill rate, and they need both fixtures at once - memdb() from the hashfile
 * tests, mkfilerec()/free_all_filerecs() from the model ones - which is why
 * they waited until both existed.
 *
 * What they get wrong fails silently in the usual way: a target elected
 * differently in two overlapping windows deduplicates into a file the other
 * window is still relocating, and the summary looks exactly the same.
 *
 * Note when reading these: the mutation tool does not mutate string literals,
 * so the GET_DUPLICATE_* queries themselves cannot be mutated. Assertions that
 * only exercise SQL filtering guard against hand edits but move no kill rate;
 * the ones that matter are on the C around them.
 * ---------------------------------------------------------------------------
 */

/* A scanned row with every column the whole-file loader ranks on. */
