/*
 * Extent maps: the layout key, the sharing walk, and the ioctl wrapper.
 *
 * Part of the oans unit suite. tests/unit/main.c includes this file along
 * with the sources it exercises, so a test still reaches a static function
 * the way it always did.
 */

MU_TEST(test_get_extent) {
	/* Three data extents with holes between them:
	 * [0, 4k)   hole   [8k, 12k)   hole   [16k, 20k) */
	struct fm_rec recs[] = {
		{0, 0, 4096, 0}, {8192, 0, 4096, 0}, {16384, 0, 4096, 0}
	};
	struct fiemap *fm = mkmap(recs, ARRAY_SIZE(recs));

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

/* Build both maps, compare, free. Keeps each case below to its records. */
MU_TEST(test_fiemap_layout_key) {
	const uint32_t SH = FIEMAP_EXTENT_SHARED;
	const uint32_t ENC = FIEMAP_EXTENT_ENCODED;
	unsigned char a[DIGEST_LEN], b[DIGEST_LEN];

	struct fm_rec two[] = {{0, 4096, 8192, 0}, {8192, 65536, 4096, 0}};

	mu_check(key_of(two, 2, 12288, a));

	/* The same layout, described identically, keys the same. */
	mu_check(key_of(two, 2, 12288, b));
	mu_check(memcmp(a, b, DIGEST_LEN) == 0);

	/* SHARED is a refcount property - deduping an unrelated file must not
	 * change what this file's content is. Same for the positional LAST. */
	struct fm_rec shared[] = {{0, 4096, 8192, SH},
				  {8192, 65536, 4096, SH | FIEMAP_EXTENT_LAST}};

	mu_check(key_of(shared, 2, 12288, b));
	mu_check(memcmp(a, b, DIGEST_LEN) == 0);

	/* Different storage, same shape: different key. */
	struct fm_rec moved[] = {{0, 4096, 8192, 0}, {8192, 69632, 4096, 0}};

	mu_check(key_of(moved, 2, 12288, b));
	mu_check(memcmp(a, b, DIGEST_LEN) != 0);

	/* Same records, different file size - a trailing hole fiemap never
	 * reports, and content the digest does cover. */
	mu_check(key_of(two, 2, 16384, b));
	mu_check(memcmp(a, b, DIGEST_LEN) != 0);

	/* A prefix of the same records must not collide with the whole. */
	mu_check(key_of(two, 1, 12288, b));
	mu_check(memcmp(a, b, DIGEST_LEN) != 0);

	/*
	 * Both sides have to sit the same distance into their record, and that
	 * has to be checked *separately* from the address rather than folded
	 * into `phys + offset` arithmetic. On a compressed extent fe_physical
	 * names the extent as a whole, so an offset into it means nothing - and
	 * the two spellings differ exactly here, where the sums coincide and
	 * the addresses do not. Believing this skips a dedupe that was real.
	 *
	 * The target record starts at the range, the destination's starts 4 KiB
	 * before it; 8192+0 and 4096+4096 are the same number and the stored
	 * extents are not the same extent.
	 */
	struct fm_rec at_start[] = {{4096, 8192, 8192, SH}};
	struct fm_rec offset_in[] = {{0, 4096, 8192, SH}};

	mu_check(!share(at_start, 1, 4096, offset_in, 1, 4096, 4096));

	/* Compressed extents are fine: the address is only ever compared. */
	struct fm_rec enc[] = {{0, 4096, 8192, ENC}};

	mu_check(key_of(enc, 1, 8192, b));

	/*
	 * More extents than the stack buffer holds, so the heap path runs.
	 * LAYOUT_KEY_STACK_EXTENTS is 32 and every fixture above has one or
	 * two, so that branch - and the record-stride arithmetic that sizes
	 * both buffers - had never been reached.
	 *
	 * Only half of a stride mismatch is visible from here, and it is worth
	 * being exact about which. A stride *wider* than the loop's leaves the
	 * moved word past `bytes` and unhashed, so the last assertion below
	 * catches it. A stride *narrower* feeds the unwritten tail of the
	 * buffer to checksum_block(), which a plain run cannot see - that one
	 * belongs to the valgrind and ASAN legs, the same shape as the missing
	 * memset in start_running_checksum().
	 */
	{
		struct fm_rec many[40];
		unsigned char big[DIGEST_LEN], big2[DIGEST_LEN];

		for (unsigned int i = 0; i < ARRAY_SIZE(many); i++) {
			many[i].log = (uint64_t)i * 8192;
			many[i].phys = (uint64_t)(i + 100) * 4096;
			many[i].len = 4096;
			many[i].flags = 0;
		}

		/* Either side of the boundary, and well past it. */
		mu_check(key_of(many, LAYOUT_KEY_STACK_EXTENTS, 1u << 20, big));
		mu_check(key_of(many, LAYOUT_KEY_STACK_EXTENTS + 1, 1u << 20, big2));
		/* Different counts key differently - which rec[1] alone
		 * guarantees, so this is a sanity check on the fixture rather
		 * than a claim about the boundary. */
		mu_assert(memcmp(big, big2, DIGEST_LEN) != 0,
			  "one extent more than the stack holds keyed the same");

		mu_check(key_of(many, ARRAY_SIZE(many), 1u << 20, big));
		mu_check(key_of(many, ARRAY_SIZE(many), 1u << 20, big2));
		mu_assert(memcmp(big, big2, DIGEST_LEN) == 0,
			  "the heap path is not deterministic");

		/* And it still notices a moved address out there, which is the
		 * whole point of hashing every record rather than a summary. */
		many[ARRAY_SIZE(many) - 1].phys += 4096;
		mu_check(key_of(many, ARRAY_SIZE(many), 1u << 20, big2));
		mu_assert(memcmp(big, big2, DIGEST_LEN) != 0,
			  "a moved address in the last record did not change the key");
	}

	/* Records whose address means nothing, or nothing stable, are refused. */
	struct fm_rec inl[] = {{0, 0, 512, FIEMAP_EXTENT_DATA_INLINE}};
	struct fm_rec delalloc[] = {{0, 0, 8192, FIEMAP_EXTENT_DELALLOC}};
	struct fm_rec unknown[] = {{0, 0, 8192, FIEMAP_EXTENT_UNKNOWN}};
	struct fm_rec crypt[] = {{0, 4096, 8192, FIEMAP_EXTENT_DATA_ENCRYPTED}};

	mu_check(!key_of(inl, 1, 512, b));
	mu_check(!key_of(delalloc, 1, 8192, b));
	mu_check(!key_of(unknown, 1, 8192, b));
	mu_check(!key_of(crypt, 1, 8192, b));

	/* One bad record poisons the whole file, not just itself. */
	struct fm_rec mixed[] = {{0, 4096, 8192, 0},
				 {8192, 0, 4096, FIEMAP_EXTENT_DELALLOC}};

	mu_check(!key_of(mixed, 2, 12288, b));

	/* A file with no extents at all has no layout to speak of. */
	struct fiemap *empty = mkmap(two, 0);

	mu_check(!fiemap_layout_key(empty, 0, b));
	free(empty);
}

MU_TEST(test_fiemap_maps_share) {
	const uint32_t SH = FIEMAP_EXTENT_SHARED;
	const uint32_t ENC = FIEMAP_EXTENT_ENCODED;

	/* Identical single records over the whole range. */
	struct fm_rec one[] = {{0, 4096, 8192, SH}};

	mu_check(share(one, 1, 0, one, 1, 0, 8192));

	/* Different stored extent: not shared. */
	struct fm_rec elsewhere[] = {{0, 8192, 8192, SH}};

	mu_check(!share(one, 1, 0, elsewhere, 1, 0, 8192));

	/*
	 * The regression: same storage, but the destination's tail is split at
	 * the block boundary a previous dedupe stopped on.
	 */
	struct fm_rec whole[] = {{0, 4096, 12288, SH | ENC}};
	struct fm_rec split[] = {{0, 4096, 8192, SH | ENC},
				 {8192, 99999, 4096, ENC}};

	mu_check(share(whole, 1, 0, split, 2, 0, 8192));
	/* ... but not once the range reaches into the split-off part. */
	mu_check(!share(whole, 1, 0, split, 2, 0, 12288));

	/*
	 * Matching holes are shared: a sparse cache file is mostly hole, so the
	 * map simply stops before the end of the range.
	 */
	mu_check(share(one, 1, 0, one, 1, 0, 262144));

	/* A hole facing data is a real difference. */
	struct fm_rec then_data[] = {{0, 4096, 8192, SH}, {8192, 8192, 4096, 0}};

	mu_check(!share(one, 1, 0, then_data, 2, 0, 12288));

	/* Interior holes must line up on both sides. */
	struct fm_rec gapped[] = {{0, 4096, 4096, SH}, {8192, 8192, 4096, SH}};
	struct fm_rec packed[] = {{0, 4096, 4096, SH}, {4096, 8192, 4096, SH}};

	mu_check(share(gapped, 2, 0, gapped, 2, 0, 12288));
	mu_check(!share(gapped, 2, 0, packed, 2, 0, 12288));

	/* No stable physical location: never shared, whatever the offsets say. */
	struct fm_rec delalloc[] = {{0, 0, 8192, FIEMAP_EXTENT_DELALLOC}};

	mu_check(!share(delalloc, 1, 0, delalloc, 1, 0, 8192));

	/*
	 * Ranges at different logical offsets, on the same stored extent at the
	 * same offset into it (the extent pass compares mid-file ranges).
	 */
	struct fm_rec at64k[] = {{65536, 4096, 8192, SH}};
	struct fm_rec at128k[] = {{131072, 4096, 8192, SH}};

	mu_check(share(at64k, 1, 65536, at128k, 1, 131072, 8192));

	/*
	 * A record that begins before the range: shared only when both sides
	 * start the same distance into the same stored extent.
	 */
	struct fm_rec big[] = {{0, 4096, 16384, SH}};
	struct fm_rec offset[] = {{4096, 4096, 12288, SH}};

	mu_check(share(big, 1, 8192, big, 1, 8192, 8192));
	mu_check(!share(big, 1, 8192, offset, 1, 8192, 4096));
}

/* Sort the target's addresses, measure the destination against them, free. */
static uint64_t unshared(const struct fm_rec *rt, unsigned int nt,
			 const struct fm_rec *rd, unsigned int nd,
			 uint64_t dest_off, uint64_t len)
{
	struct fiemap *t = mkmap(rt, nt), *d = nd ? mkmap(rd, nd) : NULL;
	struct fiemap_phys_set seen;
	uint64_t bytes;

	fiemap_phys_set_init(&seen, t);
	bytes = fiemap_unshared_bytes(&seen, d, dest_off, len);
	fiemap_phys_set_free(&seen);
	free(t);
	free(d);
	return bytes;
}

/*
 * fiemap_unshared_bytes() answers "how much would deduping this destination
 * actually stop duplicating" - the figure a run reports as reclaimed. The
 * kernel's own byte count cannot answer it: FIDEDUPERANGE reports the whole
 * compared length even for a range that already shared the target's storage
 * (#187).
 */
MU_TEST(test_fiemap_unshared_bytes) {
	const uint32_t SH = FIEMAP_EXTENT_SHARED;
	struct fm_rec tgt[] = {{0, 4096, 8192, SH}};

	/* Already on the target's extent: nothing would be freed. */
	mu_check(unshared(tgt, 1, tgt, 1, 0, 8192) == 0);

	/* A copy of its own: all of it. */
	struct fm_rec other[] = {{0, 99999, 8192, 0}};

	mu_check(unshared(tgt, 1, other, 1, 0, 8192) == 8192);

	/*
	 * The case the reported figure got wrong: half the destination already
	 * sits on the target's extent, so only the other half is duplicated.
	 */
	struct fm_rec half[] = {{0, 4096, 4096, SH}, {4096, 99999, 4096, 0}};

	mu_check(unshared(tgt, 1, half, 2, 0, 4096 * 2) == 4096);

	/* Position is irrelevant: the same stored extent frees nothing wherever
	 * the destination references it. */
	struct fm_rec elsewhere[] = {{0, 4096, 4096, SH}};

	mu_check(unshared(tgt, 1, elsewhere, 1, 0, 4096) == 0);

	/* Records are clipped to the range, not counted whole. */
	struct fm_rec wide[] = {{0, 99999, 1 << 20, 0}};

	mu_check(unshared(tgt, 1, wide, 1, 0, 8192) == 8192);

	/* A hole frees nothing - there is nothing there to stop duplicating. */
	struct fm_rec late[] = {{65536, 99999, 4096, 0}};

	mu_check(unshared(tgt, 1, late, 1, 0, 8192) == 0);

	/* No usable address on either side: cannot prove anything is shared, so
	 * report it all as duplicated rather than over-claiming a saving. */
	struct fm_rec delalloc[] = {{0, 0, 8192, FIEMAP_EXTENT_DELALLOC}};

	mu_check(unshared(tgt, 1, delalloc, 1, 0, 8192) == 8192);
	mu_check(unshared(delalloc, 1, tgt, 1, 0, 8192) == 8192);

	/* No destination map at all (fiemap failed): same fallback. */
	mu_check(unshared(tgt, 1, NULL, 0, 0, 8192) == 8192);

	/*
	 * The two measures gate different things - one a skip, one a number -
	 * but they must agree at the boundary: anything fiemap_maps_share()
	 * calls fully shared has nothing left to free. Shared fixtures, so a
	 * future relaxation of one cannot silently drift from the other.
	 */
	struct fm_rec tail[] = {{0, 4096, 8192, SH}, {8192, 99999, 4096, 0}};

	/* Identical maps. */
	mu_check(share(tgt, 1, 0, tgt, 1, 0, 8192));
	mu_check(unshared(tgt, 1, tgt, 1, 0, 8192) == 0);
	/* A tail split off past the compared range. */
	mu_check(share(tgt, 1, 0, tail, 2, 0, 8192));
	mu_check(unshared(tgt, 1, tail, 2, 0, 8192) == 0);
	/* Matching holes to the end of the range. */
	mu_check(share(tgt, 1, 0, tgt, 1, 0, 262144));
	mu_check(unshared(tgt, 1, tgt, 1, 0, 262144) == 0);
}

/*
 * Storage two destinations of one group already share with *each other* must
 * be credited once, not once each: releasing that one extent frees its length
 * once (#191). The set the measure carries is what makes that work, so drive
 * it across several destinations the way a group does.
 */
MU_TEST(test_fiemap_unshared_bytes_accumulates) {
	const uint32_t SH = FIEMAP_EXTENT_SHARED;
	struct fm_rec tgt[] = {{0, 4096, 8192, SH}};
	struct fm_rec on_b[] = {{0, 50000, 8192, SH}};
	struct fm_rec on_c[] = {{0, 90000, 8192, 0}};
	struct fiemap *t = mkmap(tgt, 1), *b = mkmap(on_b, 1), *c = mkmap(on_c, 1);
	struct fiemap_phys_set seen;

	fiemap_phys_set_init(&seen, t);

	/* First destination on extent B: its length is genuinely duplicated. */
	mu_check(fiemap_unshared_bytes(&seen, b, 0, 8192) == 8192);
	/* A second destination on the same extent frees nothing further. */
	mu_check(fiemap_unshared_bytes(&seen, b, 0, 8192) == 0);
	/* A third, on storage of its own, is credited again. */
	mu_check(fiemap_unshared_bytes(&seen, c, 0, 8192) == 8192);
	mu_check(fiemap_unshared_bytes(&seen, c, 0, 8192) == 0);
	/* The target's own storage was never creditable. */
	mu_check(fiemap_unshared_bytes(&seen, t, 0, 8192) == 0);

	/*
	 * A record with no usable address must be credited *every* time, not
	 * remembered after the first. fe_physical is zero-or-meaningless there,
	 * so remembering it would make one unwritten extent stand in for every
	 * later one - and the figure this feeds is a saving, where guessing high
	 * is the way to be wrong that a user cannot check.
	 */
	struct fm_rec pending[] = {{0, 0, 8192, FIEMAP_EXTENT_DELALLOC}};
	struct fiemap *d = mkmap(pending, 1);

	mu_check(fiemap_unshared_bytes(&seen, d, 0, 8192) == 8192);
	mu_check(fiemap_unshared_bytes(&seen, d, 0, 8192) == 8192);
	free(d);

	fiemap_phys_set_free(&seen);
	free(t);
	free(b);
	free(c);
}

/* The set has to stay sorted and unique across many merges, or bsearch starts
 * missing addresses and the over-count creeps back in one destination at a
 * time. Drive enough destinations to force it through several growths. */
MU_TEST(test_fiemap_phys_set_grows) {
	struct fiemap *t = mkmap((struct fm_rec[]){{0, 4096, 4096, 0}}, 1);
	struct fiemap_phys_set seen;

	fiemap_phys_set_init(&seen, t);
	free(t);

	for (unsigned int i = 0; i < 200; i++) {
		/* Descending addresses, so every merge prepends. */
		struct fm_rec r[] = {{0, 1000000 - i * 4096, 4096, 0}};
		struct fiemap *d = mkmap(r, 1);

		mu_check(fiemap_unshared_bytes(&seen, d, 0, 4096) == 4096);
		mu_check(fiemap_unshared_bytes(&seen, d, 0, 4096) == 0);
		free(d);
	}

	mu_check(seen.n == 201);		/* target + 200 distinct */
	for (unsigned int i = 1; i < seen.n; i++)
		mu_check(seen.v[i - 1] < seen.v[i]);	/* sorted, unique */

	fiemap_phys_set_free(&seen);
}

#define PROP_MAX_RECS	5
static unsigned int gen_layout(struct prop *p, struct fm_rec *out)
{
	unsigned int n = (unsigned int)prop_range(p, 1, PROP_MAX_RECS);
	uint64_t log = 0;

	for (unsigned int i = 0; i < n; i++) {
		if (prop_chance(p, 3))			/* a hole before it */
			log += PROP_BLOCK * prop_range(p, 1, 2);
		out[i].log = log;
		out[i].len = PROP_BLOCK * prop_range(p, 1, 3);
		/* A small pool of addresses, offset so that 0 - which fiemap
		 * uses for "nowhere" - is never one of them. */
		out[i].phys = PROP_BLOCK * prop_range(p, 100, 108);
		out[i].flags = prop_bool(p) ? FIEMAP_EXTENT_SHARED : 0;
		if (prop_chance(p, 40))
			out[i].flags |= FIEMAP_EXTENT_DELALLOC;
		log += out[i].len;
	}
	out[n - 1].flags |= FIEMAP_EXTENT_LAST;
	return n;
}

/*
 * fiemap.c's own definition, not a copy of it. This is a *gate* deciding which
 * cases a property applies to rather than an oracle, so the usual "restate it
 * independently so the test cannot agree with the bug" argument is inverted: a
 * drifted copy here makes the property vacuous or spuriously red.
 */
static bool layout_has_phys(const struct fm_rec *r, unsigned int n)
{
	for (unsigned int i = 0; i < n; i++)
		if (r[i].flags & FIEMAP_NO_PHYS)
			return false;
	return true;
}

/*
 * The shape #186 is about: one dedupe stopped on a block boundary, so the same
 * stored extent is described as two records here and one there. Returns the new
 * record count, or `n` unchanged where there is nothing to split.
 */
static unsigned int split_last_record(struct fm_rec *r, unsigned int n)
{
	if (n >= PROP_MAX_RECS || r[n - 1].len <= PROP_BLOCK)
		return n;
	r[n].log = r[n - 1].log + PROP_BLOCK;
	r[n].phys = r[n - 1].phys;
	r[n].len = r[n - 1].len - PROP_BLOCK;
	r[n].flags = r[n - 1].flags;
	r[n - 1].len = PROP_BLOCK;
	r[n - 1].flags &= ~(uint32_t)FIEMAP_EXTENT_LAST;
	return n + 1;
}

/*
 * The convergence property, from the other end than test_dedupe_idempotent.py
 * reaches it: a range always already shares storage with *itself*, so a second
 * dedupe run over an unchanged tree submits nothing. #186 was three separate
 * ways for this to be false, and what made it survive so long is that the
 * summary reports bytes compared rather than bytes freed - the run says it
 * reclaimed gigabytes either way.
 *
 * Excludes only the records whose address means nothing, which the function
 * refuses by design.
 */
MU_TEST(test_prop_maps_share_with_themselves) {
	declare_prop(p, 20000);

	while (prop_next(&p)) {
		struct fm_rec recs[PROP_MAX_RECS];
		unsigned int n = gen_layout(&p, recs);
		uint64_t end = recs[n - 1].log + recs[n - 1].len;
		uint64_t off = PROP_BLOCK * prop_below(&p, 3);
		uint64_t len = PROP_BLOCK * prop_range(&p, 1, end / PROP_BLOCK + 2);

		if (!layout_has_phys(recs, n))
			continue;
		prop_check(&p, share(recs, n, off, recs, n, off, len));
	}
}

/*
 * A record the kernel cannot pin down poisons the whole comparison, wherever
 * it sits. Deduping past one would be submitted against an address that names
 * nothing - and unlike a miss, which costs one redundant ioctl, believing it
 * skips a dedupe that was real.
 */
MU_TEST(test_prop_maps_never_share_without_a_real_address) {
	declare_prop(p, 20000);

	while (prop_next(&p)) {
		struct fm_rec recs[PROP_MAX_RECS];
		unsigned int n = gen_layout(&p, recs);
		unsigned int bad = (unsigned int)prop_below(&p, n);
		uint64_t len;

		switch (prop_below(&p, 3)) {
		case 0: recs[bad].flags |= FIEMAP_EXTENT_UNKNOWN; break;
		case 1: recs[bad].flags |= FIEMAP_EXTENT_DELALLOC; break;
		default: recs[bad].flags |= FIEMAP_EXTENT_DATA_INLINE; break;
		}
		/* Long enough to reach the poisoned record, so the walk has to
		 * meet it rather than stopping short of it. */
		len = recs[bad].log + recs[bad].len;
		prop_check(&p, !share(recs, n, 0, recs, n, 0, len));
	}
}

/*
 * The two measures gate different things - one a skip, one a reported number -
 * and the fixed tests already pin them together at a handful of points. This
 * is the same statement over whatever the generator produces: anything
 * fiemap_maps_share() calls fully shared has, by definition, nothing left to
 * free, and a figure claiming otherwise is #187 again.
 *
 * Only one direction is asserted. The converse - nothing left to free implies
 * fully shared - is not a promise either function makes: fiemap_unshared_bytes
 * matches addresses in any order while fiemap_maps_share walks the two maps in
 * step and refuses what it cannot line up, so a destination holding the
 * target's extents in a different order is 0 bytes and not shared.
 */
MU_TEST(test_prop_shared_ranges_have_nothing_left_to_free) {
	declare_prop(p, 20000);

	while (prop_next(&p)) {
		struct fm_rec tgt[PROP_MAX_RECS], dst[PROP_MAX_RECS];
		unsigned int nt = gen_layout(&p, tgt), nd;
		uint64_t len;

		/* The destination is mostly a variation on the target, because
		 * two independently drawn layouts almost never share anything
		 * and the interesting half of this property would never run. */
		nd = nt;
		memcpy(dst, tgt, nt * sizeof(*tgt));
		switch (prop_below(&p, 4)) {
		case 0:				/* identical */
			break;
		case 1:				/* one address moved */
			dst[prop_below(&p, nd)].phys += PROP_BLOCK;
			break;
		case 2:				/* a split tail */
			nd = split_last_record(dst, nd);
			break;
		default:			/* something else entirely */
			nd = gen_layout(&p, dst);
			break;
		}

		len = PROP_BLOCK * prop_range(&p, 1, 8);
		if (share(tgt, nt, 0, dst, nd, 0, len))
			prop_check(&p, unshared(tgt, nt, dst, nd, 0, len) == 0);
	}
}

/*
 * What a destination can be credited with is bounded by the range submitted,
 * and a second destination on storage already accounted for adds nothing.
 * Both are #191: crediting two destinations in full for one extent claimed
 * twice what releasing it frees.
 *
 * The second statement holds only where every record has a real address, and
 * the generator produces plenty that do not. A DELALLOC or inline record has
 * no stable address to remember, so it is never merged into the set and every
 * pass credits it again - which is the conservative direction (the figure it
 * feeds is a saving, and over-claiming one is #187) but it does mean the
 * unrestricted form of this property is false by design. What is true
 * unconditionally is that a second pass can never credit *more* than the
 * first, since it is measured against a superset of what has been seen.
 */
MU_TEST(test_prop_unshared_bytes_are_bounded_and_credited_once) {
	declare_prop(p, 20000);

	while (prop_next(&p)) {
		struct fm_rec tgt[PROP_MAX_RECS], dst[PROP_MAX_RECS];
		unsigned int nt = gen_layout(&p, tgt);
		unsigned int nd = gen_layout(&p, dst);
		uint64_t len = PROP_BLOCK * prop_range(&p, 1, 8);
		struct fiemap *t = mkmap(tgt, nt), *d = mkmap(dst, nd);
		struct fiemap_phys_set seen;
		uint64_t first, again;

		fiemap_phys_set_init(&seen, t);
		first = fiemap_unshared_bytes(&seen, d, 0, len);
		again = fiemap_unshared_bytes(&seen, d, 0, len);
		fiemap_phys_set_free(&seen);
		free(t);
		free(d);

		prop_check(&p, first <= len);
		prop_check(&p, again <= first);
		if (layout_has_phys(dst, nd))
			prop_check(&p, again == 0);
	}
}

/*
 * A tiny alphabet on purpose: with three letters and three metacharacters,
 * patterns and paths collide constantly, where a wide alphabet would generate
 * thousands of cases that match nothing and prove nothing.
 */
/* Paths tried against each compiled pattern. Compiling the automaton is nearly
 * the whole cost of a glob case, so this buys path coverage at no extra
 * regexes - and the suite's own speed matters here beyond the usual reason:
 * the mutation tool derives a mutant's hang timeout from how long a green run
 * takes, so a slow suite reclassifies slow mutants as hangs rather than
 * letting a test catch them. */
static void rows_of(const struct fm_rec *recs, unsigned int n,
		    struct extent_csum *out)
{
	for (unsigned int i = 0; i < n; i++) {
		out[i].loff = recs[i].log;
		out[i].poff = recs[i].phys;
		out[i].len = recs[i].len;
		memset(out[i].digest, (int)i + 1, DIGEST_LEN);
	}
}

/*
 * The re-check accepts exactly the layout it was given and nothing else.
 *
 * Misses are free and false hits are catastrophic: a wrong match copies one
 * file's digest onto another, and nothing downstream can tell that from a file
 * with no duplicate. So both halves are asserted for every generated layout -
 * that the records it stored match, and that no single-field change to any one
 * record still does.
 *
 * Not a tautology: one side is a `struct fiemap`, the other is the rows
 * dbfile_store_extent_hashes() wrote, and only the loop under test relates
 * them. A mutation inside it makes the two sides disagree rather than agreeing
 * with each other.
 */
MU_TEST(test_prop_a_layout_matches_only_itself) {
	declare_prop(p, 300);
	_cleanup_(sqlite3_close_cleanup) struct dbhandle *db = memdb();

	while (prop_next(&p)) {
		struct fm_rec recs[PROP_MAX_RECS], probe[PROP_MAX_RECS];
		struct extent_csum rows[PROP_MAX_RECS];
		unsigned int n = gen_layout(&p, recs);
		int64_t donor = prop_donor(db, &p);
		unsigned int k, field;

		rows_of(recs, n, rows);
		if (dbfile_store_extent_hashes(db, donor, n, rows))
			abort();

		memcpy(probe, recs, n * sizeof(*probe));
		prop_check(&p, layout_matches(db, donor, probe, n) == 1);

		/* One field of one record, moved by one block. */
		k = (unsigned int)prop_below(&p, n);
		field = (unsigned int)prop_below(&p, 3);
		if (field == 0)
			probe[k].log += PROP_BLOCK;
		else if (field == 1)
			probe[k].phys += PROP_BLOCK;
		else
			probe[k].len += PROP_BLOCK;
		prop_check(&p, layout_matches(db, donor, probe, n) == 0);

		/* A prefix of the donor is still a miss, both directions. */
		memcpy(probe, recs, n * sizeof(*probe));
		if (n > 1)
			prop_check(&p, layout_matches(db, donor, probe, n - 1) == 0);
	}
}

/*
 * The same *storage* described with different record boundaries reads as a
 * miss. Splitting one record in two covers byte for byte what the donor
 * covers, at the same addresses - so a check that reasoned about coverage
 * would say yes, and it must say no (#186 is the same confusion from the other
 * side, where treating two descriptions as different cost convergence).
 *
 * Its own property rather than a branch of the one above, because it is the
 * one case where "obviously the same bytes" and "the same records" part
 * company, and a generator has to be told to produce it.
 */
MU_TEST(test_prop_a_split_record_is_not_the_layout_it_covers) {
	declare_prop(p, 300);
	_cleanup_(sqlite3_close_cleanup) struct dbhandle *db = memdb();

	while (prop_next(&p)) {
		struct fm_rec recs[PROP_MAX_RECS], probe[PROP_MAX_RECS + 1];
		struct extent_csum rows[PROP_MAX_RECS];
		unsigned int n = gen_layout(&p, recs);
		int64_t donor;
		unsigned int k = (unsigned int)prop_below(&p, n);
		unsigned int i, j;
		uint64_t half;

		if (recs[k].len < 2 * PROP_BLOCK)
			continue;		/* nothing to split */
		half = recs[k].len / 2;

		donor = prop_donor(db, &p);
		rows_of(recs, n, rows);
		if (dbfile_store_extent_hashes(db, donor, n, rows))
			abort();

		for (i = 0, j = 0; i < n; i++) {
			probe[j++] = recs[i];
			if (i != k)
				continue;
			probe[j - 1].len = half;
			probe[j] = recs[i];
			probe[j].log += half;
			probe[j].len -= half;
			/* The address is left alone: fe_physical addresses a
			 * whole extent, so an offset into it means nothing on
			 * a compressed one. Same trap as fiemap_maps_share. */
			j++;
		}
		prop_check(&p, layout_matches(db, donor, probe, j) == 0);
	}
}

/*
 * Everything handed to dbfile_store_extent_hashes() comes back at the offsets
 * it was given, and nothing else does.
 *
 * The second clause is what makes the zero-length skip a property rather than
 * a case: an extent naming no bytes must not be stored at any count, at any
 * position in the array, including as the only element. The table above can
 * only ask that at the two shapes it writes down.
 *
 * Read back with SQL rather than through a loader, so a bind index shifted
 * consistently in both directions cannot round-trip.
 */
static void fm_close(struct fm_file *f)
{
	if (f->fd >= 0)
		close(f->fd);
	if (f->path[0])
		unlink(f->path);
}

MU_TEST(test_fiemap_maps_a_real_file) {
	_cleanup_(fm_close) struct fm_file f = fm_open(__func__, 1, false);
	_cleanup_(freep) struct fiemap *fm = NULL;
	unsigned int counted;
	uint64_t covered = 0;

	if (f.fd < 0)
		return;

	counted = fiemap_count_extents(f.fd, 0, ~0ULL);
	mu_assert(counted >= 1, "a written and fsynced file mapped no extents");

	fm = do_fiemap(f.fd);
	mu_check(fm != NULL);
	mu_assert(fm->fm_mapped_extents == counted,
		  "the mapping pass disagreed with the count pass");

	/* Ascending, non-overlapping, and covering what was written. The
	 * filesystem chooses how many records that takes. */
	for (unsigned int i = 0; i < fm->fm_mapped_extents; i++) {
		struct fiemap_extent *e = &fm->fm_extents[i];

		if (i == 0)
			mu_check(e->fe_logical == 0);
		else
			mu_assert(e->fe_logical >= fm->fm_extents[i - 1].fe_logical +
				  fm->fm_extents[i - 1].fe_length,
				  "extents came back out of order or overlapping");
		covered += e->fe_length;
	}
	mu_assert(covered >= f.size, "the extents do not cover the file");

	/*
	 * And the single-ioctl shortcut agrees with the full map. It exists so
	 * the dedupe rescan need not enumerate a huge file, so the one thing
	 * that must hold is that it answers the same as the long way round.
	 */
	{
		uint64_t poff = 0;

		mu_check(fiemap_first_extent_poff(f.fd, 0, f.size, &poff) == 0);
		mu_assert(poff == fm->fm_extents[0].fe_physical,
			  "the shortcut disagreed with the full map");
	}
}

/*
 * A range query maps the range asked for, and answers "hole" rather than
 * "error" where there is nothing.
 *
 * do_fiemap_range() returning NULL for both is deliberate and is why
 * fiemap_count_shared() can treat NULL as zero shared bytes rather than as a
 * failure - a distinction that would otherwise turn every hole into an error.
 */
MU_TEST(test_fiemap_range_answers_for_the_range_asked_for) {
	_cleanup_(fm_close) struct fm_file f = fm_open(__func__, 2, true);
	_cleanup_(freep) struct fiemap *first = NULL;
	/* Kept rather than discarded: on the branch where these assertions
	 * fail the map would otherwise leak, and the valgrind unit leg turns
	 * one clear failure into a failure plus an unrelated leak report. */
	_cleanup_(freep) struct fiemap *hole = NULL;
	_cleanup_(freep) struct fiemap *past = NULL;
	uint64_t poff = 0;

	if (f.fd < 0)
		return;

	/* The hole between the two written chunks. */
	hole = do_fiemap_range(f.fd, FM_CHUNK, FM_CHUNK);
	mu_assert(hole == NULL, "a hole was reported as an extent");
	mu_assert(fiemap_first_extent_poff(f.fd, FM_CHUNK, FM_CHUNK, &poff) == -1,
		  "the shortcut found an extent in a hole");

	/* Past the end of the file. */
	past = do_fiemap_range(f.fd, f.size + FM_CHUNK, FM_CHUNK);
	mu_assert(past == NULL, "a range past EOF was reported as an extent");

	/* The first chunk, which is there. */
	first = do_fiemap_range(f.fd, 0, FM_CHUNK);
	mu_check(first != NULL);
	mu_assert(first->fm_mapped_extents >= 1, "the first chunk mapped nothing");
	mu_check(first->fm_extents[0].fe_logical == 0);
}

/*
 * Nothing is shared in a file nobody has deduplicated, and saying otherwise
 * would credit the run with space it never freed.
 *
 * This is the counter behind the "net change in shared extents" line. It
 * cannot be tested positively here - making an extent SHARED needs reflink,
 * which this filesystem may not have - but the negative is the direction that
 * matters: a false positive inflates a figure users read as disk saved.
 */
MU_TEST(test_fiemap_counts_nothing_shared_in_a_fresh_file) {
	_cleanup_(fm_close) struct fm_file f = fm_open(__func__, 2, true);
	_cleanup_(freep) struct fiemap *fm = NULL;
	uint64_t shared = 99;

	if (f.fd < 0)
		return;

	/*
	 * Establish that there was something to look at first. Without this,
	 * `shared == 0` is equally satisfied by fiemap mapping nothing at all -
	 * fiemap_count_shared() takes an early return on a NULL map - so the
	 * test would stay green with fiemap_count_extents() stubbed to zero,
	 * and would say nothing about the SHARED test inside the loop.
	 */
	fm = do_fiemap_range(f.fd, 0, f.size);
	mu_assert(fm != NULL && fm->fm_mapped_extents >= 1,
		  "the fixture mapped no extents, so nothing below is tested");

	mu_check(fiemap_count_shared(f.fd, 0, f.size, &shared) == 0);
	mu_assert(shared == 0, "a freshly written file was reported as sharing");

	/* A range that is entirely hole answers zero rather than failing - the
	 * NULL-is-not-an-error path. */
	shared = 99;
	mu_check(fiemap_count_shared(f.fd, FM_CHUNK, FM_CHUNK * 2, &shared) == 0);
	mu_check(shared == 0);
}
