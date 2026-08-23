/*
 * Running-checksum save and restore (#159).
 *
 * Compiled as part of tu_csum.c, which is where the sources these tests reach
 * into are #included.
 */

MU_TEST(test_running_checksum_survives_save_restore) {
	unsigned char data[8192];
	unsigned char whole[DIGEST_LEN], resumed[DIGEST_LEN];
	_cleanup_(freep) void *blob = malloc(running_checksum_state_size());
	struct running_checksum *c;

	for (unsigned int i = 0; i < sizeof(data); i++)
		data[i] = (unsigned char)(i * 31 + (i >> 3));

	c = start_running_checksum();
	add_to_running_checksum(c, data, sizeof(data));
	finish_running_checksum(c, whole);

	/*
	 * Split at 1000 bytes: not a multiple of XXH3's 256-byte internal
	 * buffer, so the state carries buffered bytes across the break - the
	 * case a naive "just keep the accumulators" save would get wrong.
	 */
	c = start_running_checksum();
	add_to_running_checksum(c, data, 1000);
	mu_check(running_checksum_save(c, blob, running_checksum_state_size()) == 0);
	finish_running_checksum(c, NULL);

	c = running_checksum_restore(blob, running_checksum_state_size());
	mu_check(c != NULL);
	add_to_running_checksum(c, data + 1000, sizeof(data) - 1000);
	finish_running_checksum(c, resumed);

	mu_check(memcmp(whole, resumed, DIGEST_LEN) == 0);
}

/*
 * A restored state must not follow the pointer it was saved with.
 * XXH3_state_t holds one to the library's static secret, whose address is
 * whatever this process's loader chose - so the saved copy is meaningless in
 * the process that reads it back, and every checkpoint is read back by a
 * different process than wrote it. Nothing in-process can notice: the two
 * addresses are the same one until the blob crosses a process boundary, which
 * is why the saved pointer is scribbled on here rather than trusted to differ.
 */
MU_TEST(test_running_checksum_repoints_the_secret_on_restore) {
	unsigned char data[512];
	unsigned char whole[DIGEST_LEN], resumed[DIGEST_LEN];
	size_t len = running_checksum_state_size();
	_cleanup_(freep) unsigned char *blob = malloc(len);
	struct running_checksum *c;

	for (unsigned int i = 0; i < sizeof(data); i++)
		data[i] = (unsigned char)(i * 7);

	c = start_running_checksum();
	add_to_running_checksum(c, data, sizeof(data));
	finish_running_checksum(c, whole);

	c = start_running_checksum();
	add_to_running_checksum(c, data, 300);
	mu_check(running_checksum_save(c, blob, len) == 0);
	finish_running_checksum(c, NULL);

	/* What another process's copy of the same state looks like from here. */
	memset(blob + sizeof(struct csum_state_hdr) +
	       offsetof(XXH3_state_t, extSecret), 0xa5, sizeof(void *));

	c = running_checksum_restore(blob, len);
	mu_check(c != NULL);
	add_to_running_checksum(c, data + 300, sizeof(data) - 300);
	finish_running_checksum(c, resumed);
	mu_check(memcmp(whole, resumed, DIGEST_LEN) == 0);
}

/*
 * A buffer that cannot hold the state is refused and left alone. The caller is
 * the checkpoint writer, and a short buffer there would otherwise be a write
 * past whatever it had allocated.
 */
MU_TEST(test_running_checksum_save_refuses_a_short_buffer) {
	size_t len = running_checksum_state_size();
	_cleanup_(freep) unsigned char *buf = malloc(len);
	struct running_checksum *c = start_running_checksum();

	memset(buf, 0x5a, len);
	mu_check(running_checksum_save(c, buf, len - 1) != 0);
	for (size_t i = 0; i < len; i++)
		mu_check(buf[i] == 0x5a);	/* nothing was written */

	/* The control: one byte more and it is written in full. */
	mu_check(running_checksum_save(c, buf, len) == 0);
	mu_check(buf[0] != 0x5a || buf[1] != 0x5a);
	finish_running_checksum(c, NULL);
}

/* A blob this binary cannot vouch for must be refused, not reinterpreted. */
MU_TEST(test_running_checksum_rejects_foreign_state) {
	size_t len = running_checksum_state_size();
	_cleanup_(freep) unsigned char *blob = malloc(len);
	struct running_checksum *c = start_running_checksum();

	mu_check(running_checksum_save(c, blob, len) == 0);
	finish_running_checksum(c, NULL);

	/* The control: unmodified, this one has to be accepted. */
	c = running_checksum_restore(blob, len);
	mu_check(c != NULL);
	finish_running_checksum(c, NULL);

	/* A different xxhash - what a distro upgrade leaves behind. */
	blob[8] ^= 0xff;
	mu_check(running_checksum_restore(blob, len) == NULL);
	blob[8] ^= 0xff;

	/* Truncated, or from a build whose state struct was a different size. */
	mu_check(running_checksum_restore(blob, len - 1) == NULL);

	/* Not one of ours at all. */
	blob[0] ^= 0xff;
	mu_check(running_checksum_restore(blob, len) == NULL);
}

/* --- the hashfile (dbfile.c) ---
 *
 * These run against an in-memory SQLite database, which oans already supports:
 * `dbfile_open_handle(NULL)` is the same path a run without `--hashfile` takes,
 * so nothing here is a test-only seam. That is what makes dbfile.c reachable
 * from the unit suite at all - it needs no filesystem, no btrfs, and no scan.
 *
 * What is worth testing here is not "does SQLite work" but the handful of
 * invariants this file has actually broken before, each of which fails
 * *silently*: a hashfile that empties itself while the run exits 0, a resumed
 * file whose stored hashes vanish, a replay that adopts the wrong default.
 */

/*
 * A fresh in-memory hashfile.
 *
 * Nothing is cleared, because there is nothing to clear: a shared-cache
 * in-memory database exists only while a connection is open, so the moment the
 * last handle closes SQLite frees it, tables and all. Every caller below takes
 * its handle with `_cleanup_(sqlite3_close_cleanup)`, which is what makes that
 * true even when an assertion fails partway - `mu_check()` expands to a bare
 * `return`, so a hand-written close is skipped exactly when a test goes red.
 * Isolation by construction rather than by a hand-maintained `delete from`
 * list, which goes stale silently the first time the schema grows a table.
 *
 * Costs one schema build and 30 prepared statements per test, measured at
 * 1.6 ms. That is 5% of the suite and 0.07% of the mutation tool's hang
 * timeout, which is the budget that actually binds - a shared handle would buy
 * the 13 ms back and put isolation on the stale list instead.
 */
MU_TEST(test_prop_checksum_resumes_at_any_split) {
	declare_prop(p, 2000);
	unsigned char data[4096];
	size_t blob_len = running_checksum_state_size();
	_cleanup_(freep) void *blob = malloc(blob_len);

	/* Filled once: this property is about *where* a hash was interrupted,
	 * not about content, and drawing 4 KiB per case cost ten times the
	 * hashing it was there to exercise. */
	prop_bytes(&p, data, sizeof(data));

	while (prop_next(&p)) {
		unsigned char whole[DIGEST_LEN], resumed[DIGEST_LEN];
		size_t len = (size_t)prop_below(&p, sizeof(data) + 1);
		unsigned int splits = (unsigned int)prop_below(&p, 4);
		struct running_checksum *c;
		size_t at = 0;

		c = start_running_checksum();
		add_to_running_checksum(c, data, len);
		finish_running_checksum(c, whole);

		c = start_running_checksum();
		for (unsigned int s = 0; s < splits; s++) {
			/* Biased to the offsets that are awkward for a
			 * buffered hash: nothing yet, one byte, and either
			 * side of XXH3's 256-byte block. */
			size_t next;

			switch (prop_below(&p, 4)) {
			case 0: next = at; break;
			case 1: next = at + 1; break;
			case 2: next = (at + 256) & ~(size_t)255; break;
			default: next = (size_t)prop_range(&p, at, len); break;
			}
			if (next > len)
				next = len;
			add_to_running_checksum(c, data + at, next - at);
			at = next;

			prop_check(&p, running_checksum_save(c, blob, blob_len) == 0);
			finish_running_checksum(c, NULL);
			c = running_checksum_restore(blob, blob_len);
			prop_check(&p, c != NULL);
		}
		add_to_running_checksum(c, data + at, len - at);
		finish_running_checksum(c, resumed);

		prop_check(&p, memcmp(whole, resumed, DIGEST_LEN) == 0);
	}
}

/*
 * A checkpoint this binary cannot vouch for has to be refused rather than
 * reinterpreted: restoring a state a different xxhash wrote yields a digest of
 * bytes that never existed, and nothing downstream could tell that from a file
 * with no duplicate. Every bit of the header is part of that promise, so every
 * bit is flipped.
 */
MU_TEST(test_prop_checksum_refuses_any_damaged_header) {
	declare_prop(p, 4000);
	size_t blob_len = running_checksum_state_size();
	size_t hdr_len = sizeof(struct csum_state_hdr);
	_cleanup_(freep) unsigned char *blob = malloc(blob_len);

	while (prop_next(&p)) {
		struct running_checksum *c = start_running_checksum();
		size_t byte = (size_t)prop_below(&p, hdr_len);
		unsigned char bit = (unsigned char)(1u << prop_below(&p, 8));
		unsigned char fed[] = "some bytes";

		add_to_running_checksum(c, fed, sizeof(fed));
		prop_check(&p, running_checksum_save(c, blob, blob_len) == 0);
		finish_running_checksum(c, NULL);

		/* The control: untouched, this one has to be accepted, or the
		 * assertion below would hold for a blob that was never valid. */
		c = running_checksum_restore(blob, blob_len);
		prop_check(&p, c != NULL);
		finish_running_checksum(c, NULL);

		blob[byte] ^= bit;
		prop_check(&p, running_checksum_restore(blob, blob_len) == NULL);
		blob[byte] ^= bit;

		/* A length that is not exactly right is refused too - a short
		 * read of a checkpoint row must not be reinterpreted. */
		prop_check(&p, running_checksum_restore(blob, blob_len - 1) == NULL);
		prop_check(&p, running_checksum_restore(blob, blob_len + 1) == NULL);
	}
}

/* Somewhere between "one extent" and "a shredded file", in whole blocks. */
