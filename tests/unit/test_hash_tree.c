/*
 * The in-memory block tree and results tree.
 *
 * Part of the oans unit suite. tests/unit/main.c includes this file along
 * with the sources it exercises, so a test still reaches a static function
 * the way it always did.
 */

#define PROP_DIGESTS	5
MU_TEST(test_prop_the_hash_tree_counts_what_it_holds) {
	declare_prop(p, 120);
	struct filerec *files[PROP_FILES];
	unsigned char digests[PROP_DIGESTS][DIGEST_LEN];

	/* Loop-invariant: the tree is torn down each iteration, and that walks
	 * every block out of the filerecs' own trees, so these come back
	 * clean. */
	free_all_filerecs();
	for (unsigned int i = 0; i < PROP_FILES; i++)
		files[i] = mkfilerec((int64_t)i + 1);
	for (unsigned int i = 0; i < PROP_DIGESTS; i++)
		digest_of(digests[i], i);

	while (prop_next(&p)) {
		struct hash_tree tree;
		unsigned int per_digest[PROP_DIGESTS] = {0};
		unsigned int nb = (unsigned int)prop_range(&p, 1, PROP_BLOCKS);
		unsigned int distinct = 0;
		uint64_t next_loff[PROP_FILES] = {0};

		init_hash_tree(&tree);

		for (unsigned int i = 0; i < nb; i++) {
			unsigned int f = (unsigned int)prop_below(&p, PROP_FILES);
			unsigned int d = (unsigned int)prop_below(&p, PROP_DIGESTS);

			/* Distinct offsets per file: the filerec block tree is
			 * keyed on the offset, and two blocks of one file at one
			 * offset is not a shape the scan produces. */
			if (insert_hashed_block(&tree, digests[d], files[f],
						next_loff[f]))
				abort();
			next_loff[f] += PROP_LEN;
			if (per_digest[d]++ == 0)
				distinct++;
		}

		prop_check(&p, tree.num_blocks == nb);
		prop_check(&p, tree.num_hashes == distinct);

		for (unsigned int d = 0; d < PROP_DIGESTS; d++) {
			struct dupe_blocks_list *dl =
				find_block_list(&tree, digests[d]);

			if (!per_digest[d]) {
				/* Never inserted: not answered with a
				 * neighbouring digest's list. */
				prop_check(&p, dl == NULL);
				continue;
			}
			prop_check(&p, dl != NULL);
			prop_check(&p, dl->dl_num_elem == per_digest[d]);
			prop_check(&p, !memcmp(dl->dl_hash, digests[d], DIGEST_LEN));
		}

		for (unsigned int f = 0; f < PROP_FILES; f++) {
			for (uint64_t off = 0; off < next_loff[f]; off += PROP_LEN) {
				struct file_block *b =
					find_filerec_block(files[f], off);

				prop_check(&p, b != NULL);
				prop_check(&p, b->b_loff == off);
				prop_check(&p, b->b_file == files[f]);
			}
			prop_check(&p, find_filerec_block(files[f],
							  next_loff[f]) == NULL);
		}

		free_hash_tree(&tree);
	}
	free_all_filerecs();
}

/*
 * Taking blocks back out, in an order unrelated to the one they went in.
 *
 * Two things here that a simpler shape cannot see. remove_hashed_block answers
 * 1 exactly when it removed the *last* block carrying that digest and freed
 * the list - find_dupes uses that to know the group is gone, so an answer
 * right about the tree and wrong about the verdict leaves a caller holding a
 * freed list. And a file_hash_head is freed when *that file's* sublist drains,
 * which with a single file would always coincide with the list itself being
 * freed; several files make the two instants different, so dropping the head
 * free leaks it somewhere a counter can still notice.
 *
 * The removal order is generated, which is also the only thing in these tests
 * that makes the vendored rbtree rebalance on erase rather than unlink a
 * spine.
 */
MU_TEST(test_prop_removing_every_block_empties_the_hash_tree) {
	declare_prop(p, 120);
	struct filerec *files[PROP_FILES];
	unsigned char digests[PROP_DIGESTS][DIGEST_LEN];

	free_all_filerecs();
	for (unsigned int i = 0; i < PROP_FILES; i++)
		files[i] = mkfilerec((int64_t)i + 1);
	for (unsigned int i = 0; i < PROP_DIGESTS; i++)
		digest_of(digests[i], i);

	while (prop_next(&p)) {
		struct hash_tree tree;
		unsigned int per_digest[PROP_DIGESTS] = {0};
		unsigned int per_pair[PROP_DIGESTS][PROP_FILES] = {{0}};
		unsigned int wd[PROP_BLOCKS], wf[PROP_BLOCKS];
		uint64_t order[PROP_BLOCKS];
		unsigned int nb = (unsigned int)prop_range(&p, 1, PROP_BLOCKS);
		unsigned int distinct = 0;
		uint64_t next_loff[PROP_FILES] = {0};
		uint64_t loffs[PROP_BLOCKS];

		init_hash_tree(&tree);

		for (unsigned int i = 0; i < nb; i++) {
			unsigned int f = (unsigned int)prop_below(&p, PROP_FILES);
			unsigned int d = (unsigned int)prop_below(&p, PROP_DIGESTS);

			wd[i] = d;
			wf[i] = f;
			loffs[i] = next_loff[f];
			order[i] = i;
			if (insert_hashed_block(&tree, digests[d], files[f],
						next_loff[f]))
				abort();
			next_loff[f] += PROP_LEN;
			per_pair[d][f]++;
			if (per_digest[d]++ == 0)
				distinct++;
		}

		prop_shuffle_u64(&p, order, nb);

		for (unsigned int k = 0; k < nb; k++) {
			unsigned int i = (unsigned int)order[k];
			unsigned int d = wd[i], f = wf[i];
			struct dupe_blocks_list *dl =
				find_block_list(&tree, digests[d]);
			struct file_block *b =
				find_filerec_block(files[f], loffs[i]);
			bool last_of_digest = per_digest[d] == 1;
			bool last_of_pair = per_pair[d][f] == 1;
			int ret;

			prop_check(&p, dl != NULL);
			prop_check(&p, b != NULL);

			per_digest[d]--;
			per_pair[d][f]--;
			ret = remove_hashed_block(&tree, b);
			prop_check(&p, ret == (last_of_digest ? 1 : 0));

			/* That file's head is gone once its share drains, while
			 * the list itself lives on for the other files. */
			if (!last_of_digest)
				prop_check(&p,
					   (find_file_hash_head(dl, files[f])
					    == NULL) == last_of_pair);

			if (last_of_digest)
				distinct--;
			prop_check(&p, tree.num_hashes == distinct);
			prop_check(&p, tree.num_blocks == nb - k - 1);
			prop_check(&p, find_filerec_block(files[f],
							  loffs[i]) == NULL);
		}

		prop_check(&p, tree.num_blocks == 0 && tree.num_hashes == 0);
		prop_check(&p, RB_EMPTY_ROOT(&tree.root));
		free_hash_tree(&tree);
	}
	free_all_filerecs();
}

/*
 * find_dupes walks each file's blocks under a hash expecting increasing
 * offsets, and says so where sort_file_hash_heads is declared. The blocks
 * arrive that way only because the scan happens to produce them so, which is
 * why the sort exists - and why a fixture that inserts in order cannot tell a
 * working sort from no sort at all. Hence a shuffled insertion order.
 *
 * Two digests, not one: the sort is two nested walks, and with a single block
 * list the outer one runs exactly once for the life of the property, so a
 * mutant that stops after the first list would be invisible.
 */
MU_TEST(test_prop_sorting_puts_every_hash_head_in_offset_order) {
	declare_prop(p, 120);
	struct filerec *files[PROP_FILES];
	unsigned char digests[2][DIGEST_LEN];

	free_all_filerecs();
	for (unsigned int i = 0; i < PROP_FILES; i++)
		files[i] = mkfilerec((int64_t)i + 1);
	digest_of(digests[0], 0);
	digest_of(digests[1], 1);

	while (prop_next(&p)) {
		struct hash_tree tree;
		uint64_t offs[PROP_BLOCKS];
		unsigned int nb = (unsigned int)prop_range(&p, 2, PROP_BLOCKS);
		unsigned int heads = 0;
		struct rb_node *dn;

		init_hash_tree(&tree);
		for (unsigned int i = 0; i < nb; i++)
			offs[i] = PROP_LEN * i;
		prop_shuffle_u64(&p, offs, nb);

		for (unsigned int i = 0; i < nb; i++) {
			unsigned int f = (unsigned int)prop_below(&p, PROP_FILES);
			unsigned int d = (unsigned int)prop_below(&p, 2);

			if (insert_hashed_block(&tree, digests[d], files[f],
						offs[i]))
				abort();
		}

		sort_file_hash_heads(&tree);

		for (dn = rb_first(&tree.root); dn; dn = rb_next(dn)) {
			struct dupe_blocks_list *dl =
				rb_entry(dn, struct dupe_blocks_list, dl_node);
			struct rb_node *n;

			for (n = rb_first(&dl->dl_files_root); n; n = rb_next(n)) {
				struct file_hash_head *h =
					rb_entry(n, struct file_hash_head, h_node);
				struct file_block *b;
				uint64_t prev = 0;
				bool first = true;

				heads++;
				list_for_each_entry(b, &h->h_blocks, b_head_list) {
					prop_check(&p, b->b_file == h->h_file);
					prop_check(&p, first || b->b_loff > prev);
					prev = b->b_loff;
					first = false;
				}
			}
		}
		/* Every list reached, not just the first. */
		prop_check(&p, heads >= tree.num_hashes);

		free_hash_tree(&tree);
	}
	free_all_filerecs();
}

/*
 * A results tree's groups and their members stay consistent with what was
 * inserted, across several digests *and* several lengths.
 *
 * A group is keyed on (digest, length) together, so drawing both is what makes
 * the keying observable at all - with one digest at one length every case
 * builds a one-node tree, num_dupes is never in question, and the comparator's
 * second level never runs.
 *
 * dext_work() is the single definition of a group's work and four consumers
 * depend on them agreeing: the largest-first sort key, the per-thread status
 * total, the byte-progress settlement target and the upfront SQL total. It is
 * restated here as arithmetic rather than called, so a change to the formula
 * makes the two sides disagree instead of moving together.
 */
MU_TEST(test_prop_a_dup_group_counts_its_own_members) {
	declare_prop(p, 150);
#define PROP_LENS	3
	struct filerec *files[PROP_FILES];
	unsigned char digests[PROP_DIGESTS][DIGEST_LEN];
	static const uint64_t lens[PROP_LENS] = {
		PROP_LEN, PROP_LEN * 2, PROP_LEN * 3
	};

	free_all_filerecs();
	for (unsigned int i = 0; i < PROP_FILES; i++)
		files[i] = mkfilerec((int64_t)i + 1);
	for (unsigned int i = 0; i < PROP_DIGESTS; i++)
		digest_of(digests[i], i);

	while (prop_next(&p)) {
		struct results_tree res;
		unsigned int n = (unsigned int)prop_range(&p, 1, 16);
		unsigned int unique = 0, groups = 0;
		/* Shadow model: which (digest, len) groups exist, and which
		 * (digest, len, file, slot) extents are in them. */
		bool has_group[PROP_DIGESTS][PROP_LENS] = {{false}};
		bool seen[PROP_DIGESTS][PROP_LENS][PROP_FILES][4];
		unsigned int members[PROP_DIGESTS][PROP_LENS] = {{0}};
		struct rb_node *node;

		init_results_tree(&res);
		memset(seen, 0, sizeof(seen));

		for (unsigned int i = 0; i < n; i++) {
			unsigned int d = (unsigned int)prop_below(&p, PROP_DIGESTS);
			unsigned int l = (unsigned int)prop_below(&p, PROP_LENS);
			unsigned int f = (unsigned int)prop_below(&p, PROP_FILES);
			unsigned int slot = (unsigned int)prop_below(&p, 4);

			/*
			 * Repeats on purpose: the same (file, offset) twice is
			 * one extent, not two, and the second insert must free
			 * its own allocation rather than counting it.
			 */
			if (insert_one_result(&res, digests[d], files[f],
					      slot * PROP_LEN * 4, lens[l],
					      0, false))
				abort();
			if (!has_group[d][l]) {
				has_group[d][l] = true;
				groups++;
			}
			if (!seen[d][l][f][slot]) {
				seen[d][l][f][slot] = true;
				members[d][l]++;
				unique++;
			}
		}

		prop_check(&p, res.num_extents == unique);
		prop_check(&p, res.num_dupes == groups);

		/* Each group is findable under its own key, and holds exactly
		 * the members the model says. */
		for (unsigned int d = 0; d < PROP_DIGESTS; d++) {
			for (unsigned int l = 0; l < PROP_LENS; l++) {
				struct dupe_extents *g =
					find_dupe_extents(&res, digests[d], lens[l]);

				if (!has_group[d][l]) {
					prop_check(&p, g == NULL);
					continue;
				}
				prop_check(&p, g != NULL);
				prop_check(&p, g->de_num_dupes == members[d][l]);
				prop_check(&p, g->de_len == lens[l]);
			}
		}

		/* The list, the rbtree and the counter are three views of one
		 * membership and must never disagree. */
		for (node = rb_first(&res.root); node; node = rb_next(node)) {
			struct dupe_extents *d =
				rb_entry(node, struct dupe_extents, de_node);
			struct extent *e;
			unsigned int listed = 0, in_tree = 0;
			struct rb_node *m;

			list_for_each_entry(e, &d->de_extents, e_list) {
				prop_check(&p, e->e_parent == d);
				listed++;
			}
			for (m = rb_first(&d->de_extents_root); m; m = rb_next(m))
				in_tree++;

			prop_check(&p, listed == d->de_num_dupes);
			prop_check(&p, in_tree == d->de_num_dupes);
			prop_check(&p, dext_work(d) ==
				   d->de_len * (d->de_num_dupes - 1));
		}

		free_results_tree(&res);
	}
	free_all_filerecs();
}

/*
 * remove_extent's cascade, which is the part a fixture gets wrong.
 *
 * A group of one is meaningless - there is nothing left to dedupe against - so
 * dropping to one member removes that member too and frees the group. Removing
 * from a pair therefore takes *both* extents and answers 0, where a straight
 * decrement would say 1. A test written against a three-member group alone
 * never reaches the cascade.
 */
MU_TEST(test_removing_an_extent_collapses_a_group_of_one) {
	struct results_tree res;
	unsigned char digest[DIGEST_LEN];
	struct dupe_extents *d;
	struct extent *e;

	free_all_filerecs();
	init_results_tree(&res);
	digest_of(digest, 1);

	for (unsigned int i = 0; i < 3; i++)
		mu_check(insert_one_result(&res, digest, mkfilerec(i + 1), 0,
					   PROP_LEN, 0, false) == 0);
	mu_check(res.num_extents == 3 && res.num_dupes == 1);

	/* Three members: one comes out, two remain, and it says so. */
	d = find_dupe_extents(&res, digest, PROP_LEN);
	mu_check(d && d->de_num_dupes == 3);
	e = list_first_entry(&d->de_extents, struct extent, e_list);
	mu_check(remove_extent(&res, e) == 2);
	mu_check(res.num_extents == 2);
	mu_check(res.num_dupes == 1);		/* the group survives */

	/* Two members: removing one leaves a group of one, which goes too. */
	d = find_dupe_extents(&res, digest, PROP_LEN);
	mu_check(d != NULL);
	e = list_first_entry(&d->de_extents, struct extent, e_list);
	mu_check(remove_extent(&res, e) == 0);
	mu_check(res.num_extents == 0);
	mu_check(res.num_dupes == 0);
	mu_check(RB_EMPTY_ROOT(&res.root));
	mu_check(find_dupe_extents(&res, digest, PROP_LEN) == NULL);

	free_results_tree(&res);
	free_all_filerecs();
}

/*
 * Groups are keyed on (digest, length) together, not on digest alone: two runs
 * of identical bytes at different lengths are not duplicates of each other,
 * and insert_one_result abort_on()s a length disagreeing with the group it
 * landed in.
 */
MU_TEST(test_a_group_is_keyed_on_digest_and_length_together) {
	struct results_tree res;
	unsigned char digest[DIGEST_LEN], other[DIGEST_LEN];
	struct filerec *a, *b;

	free_all_filerecs();
	init_results_tree(&res);
	a = mkfilerec(1);
	b = mkfilerec(2);
	digest_of(digest, 1);
	digest_of(other, 2);

	mu_check(insert_one_result(&res, digest, a, 0, PROP_LEN, 0, false) == 0);
	mu_check(insert_one_result(&res, digest, b, 0, PROP_LEN, 0, false) == 0);
	mu_check(res.num_dupes == 1);

	/* Same digest, different length: a second group, not an abort. */
	mu_check(insert_one_result(&res, digest, a, PROP_LEN * 2, PROP_LEN * 2,
				   0, false) == 0);
	mu_check(res.num_dupes == 2);

	/* Different digest, same length: a third. */
	mu_check(insert_one_result(&res, other, a, PROP_LEN * 8, PROP_LEN, 0,
				   false) == 0);
	mu_check(res.num_dupes == 3);
	mu_check(res.num_extents == 4);

	/* Each is findable under its own key and not under the other's. */
	mu_check(find_dupe_extents(&res, digest, PROP_LEN) != NULL);
	mu_check(find_dupe_extents(&res, digest, PROP_LEN * 2) != NULL);
	mu_check(find_dupe_extents(&res, other, PROP_LEN) != NULL);
	mu_check(find_dupe_extents(&res, other, PROP_LEN * 2) == NULL);

	free_results_tree(&res);
	free_all_filerecs();
}

/*
 * The anchor flag latches: once a member claims it, later members that do not
 * cannot clear it. That is what pins a group spanning several dedupe passes to
 * one target instead of re-picking a least-fragmented one per pass.
 *
 * The order matters, and it is the whole test. Asserting it right after the
 * anchored insert would pass just as well against `de_anchored = is_anchor`,
 * because nothing follows to overwrite it - so "sticks" is only a claim if a
 * non-anchored insert comes afterwards.
 */
MU_TEST(test_the_anchor_flag_latches_once_claimed) {
	struct results_tree res;
	unsigned char digest[DIGEST_LEN];
	struct dupe_extents *d;

	free_all_filerecs();
	init_results_tree(&res);
	digest_of(digest, 1);

	mu_check(insert_one_result(&res, digest, mkfilerec(1), 0, PROP_LEN, 0,
				   false) == 0);
	d = find_dupe_extents(&res, digest, PROP_LEN);
	mu_check(d && !d->de_anchored);

	mu_check(insert_one_result(&res, digest, mkfilerec(2), 0, PROP_LEN, 0,
				   true) == 0);
	mu_check(d->de_anchored);

	/* The one that matters: a later plain member must not clear it. */
	mu_check(insert_one_result(&res, digest, mkfilerec(3), 0, PROP_LEN, 0,
				   false) == 0);
	mu_check(d->de_anchored);

	free_results_tree(&res);
	free_all_filerecs();
}

/*
 * insert_result() is the pair-wise entry point find_dupes actually calls;
 * insert_one_result() above is the single-extent variant. Testing the helper
 * and not this one left 36 of results-tree.c's mutants untouched.
 *
 * Two things here that only this signature has. The group's length comes from
 * `endoff[0] - startoff[0] + 1` - **inclusive**, and taken from the *first*
 * pair alone - so an off-by-one silently files the pair under a neighbouring
 * key, where it can never meet the extents it duplicates. And both extents go
 * into one group, so a pair that is really the same extent twice must count
 * once.
 */
MU_TEST(test_insert_result_files_a_pair_under_one_length) {
	struct results_tree res;
	unsigned char digest[DIGEST_LEN];
	struct filerec *recs[2];
	uint64_t startoff[2], endoff[2];
	struct dupe_extents *d;

	free_all_filerecs();
	init_results_tree(&res);
	digest_of(digest, 1);
	recs[0] = mkfilerec(1);
	recs[1] = mkfilerec(2);

	/* An inclusive range of exactly PROP_LEN bytes. */
	startoff[0] = 0;
	endoff[0] = PROP_LEN - 1;
	startoff[1] = PROP_LEN * 4;
	endoff[1] = PROP_LEN * 5 - 1;
	mu_check(insert_result(&res, digest, recs, startoff, endoff) == 0);

	mu_check(res.num_dupes == 1);
	mu_check(res.num_extents == 2);
	d = find_dupe_extents(&res, digest, PROP_LEN);
	mu_check(d != NULL);			/* not PROP_LEN - 1, nor + 1 */
	mu_check(d && d->de_num_dupes == 2);
	mu_check(find_dupe_extents(&res, digest, PROP_LEN - 1) == NULL);
	mu_check(find_dupe_extents(&res, digest, PROP_LEN + 1) == NULL);

	/* The second file's offset is its own, not a copy of the first's. */
	{
		struct extent *e;
		bool at_0 = false, at_4 = false;

		list_for_each_entry(e, &d->de_extents, e_list) {
			if (e->e_file == recs[0] && e->e_loff == 0)
				at_0 = true;
			if (e->e_file == recs[1] && e->e_loff == PROP_LEN * 4)
				at_4 = true;
		}
		mu_check(at_0 && at_4);
	}

	/* The same extent named twice is one member, not two. */
	recs[1] = recs[0];
	startoff[1] = startoff[0];
	endoff[1] = endoff[0];
	mu_check(insert_result(&res, digest, recs, startoff, endoff) == 0);
	mu_check(res.num_extents == 2);		/* unchanged */
	mu_check(d->de_num_dupes == 2);

	free_results_tree(&res);
	free_all_filerecs();
}

/* A real file to open. Any filesystem will do - this is fd bookkeeping, not
 * dedupe - so it does not need the reflink scratch dir. */
