/*
 * Fixture builders shared by more than one subject.
 *
 * A helper earns a place here by being wanted by more than one test file;
 * anything a single subject uses lives beside that subject instead. The order
 * is the order they were written in, which is what keeps each one declared
 * before whatever uses it.
 */

#ifndef OANS_TEST_FIXTURES_H
#define OANS_TEST_FIXTURES_H

/*
 * Every fixture is `static` and marked unused: each translation unit that
 * includes this gets its own copy of the ones it calls, and the linker drops
 * the rest. Without the attribute a TU using half of them reports the other
 * half under -Wunused-function, which -Werror then turns into a broken build.
 */
#if defined(__GNUC__) || defined(__clang__)
#  define FIXTURE __attribute__((unused)) static
#else
#  define FIXTURE static
#endif

struct fm_rec { uint64_t log, phys, len; uint32_t flags; };

FIXTURE struct fiemap *mkmap(const struct fm_rec *recs, unsigned int n)
{
	struct fiemap *fm = calloc(1, sizeof(*fm) +
				   n * sizeof(struct fiemap_extent));

	fm->fm_mapped_extents = n;
	for (unsigned int i = 0; i < n; i++) {
		fm->fm_extents[i].fe_logical = recs[i].log;
		fm->fm_extents[i].fe_physical = recs[i].phys;
		fm->fm_extents[i].fe_length = recs[i].len;
		fm->fm_extents[i].fe_flags = recs[i].flags;
	}
	return fm;
}

FIXTURE bool share(const struct fm_rec *ra, unsigned int na, uint64_t off_a,
		  const struct fm_rec *rb, unsigned int nb, uint64_t off_b,
		  uint64_t len)
{
	struct fiemap *a = mkmap(ra, na), *b = mkmap(rb, nb);
	bool shared = fiemap_maps_share(a, off_a, b, off_b, len);

	free(a);
	free(b);
	return shared;
}

/*
 * fiemap_maps_share() decides whether deduping one range against another would
 * be a no-op. It has to answer "yes" for storage that is genuinely shared but
 * described with different record boundaries, or the same file is resubmitted
 * to the kernel on every run (#186) - and "no" whenever it cannot prove it.
 */
/* Same records, same size -> same key; anything that changes the bytes doesn't. */
FIXTURE bool key_of(const struct fm_rec *recs, unsigned int n, uint64_t size,
		   unsigned char *out)
{
	struct fiemap *fm = mkmap(recs, n);
	bool ok = fiemap_layout_key(fm, size, out);

	free(fm);
	return ok;
}

FIXTURE bool near(double got, double want, double eps)
{
	double e = got - want;
	return e < eps && e > -eps;
}

FIXTURE struct dbhandle *memdb(void)
{
	struct dbhandle *db = dbfile_open_handle(NULL);

	if (!db)
		abort();	/* the test cannot run at all, not a finding */
	return db;
}

/*
 * Row count for the tables no shipped API reports. `files`, `blocks` and
 * `extents` deliberately go through dbfile_get_stats() instead - that is what
 * `--stats` and `--json` print, so asserting through it covers the production
 * counters as well as the tables.
 */
/*
 * A statement whose only job is to set the fixture up. It aborts rather than
 * mu_check()s: a mistyped column name makes sqlite3_exec() a no-op, and a
 * fixture that quietly did nothing is a test that quietly proves nothing -
 * which is how the wrong-length-blob case below first passed against a column
 * that does not exist.
 */
FIXTURE void exec(struct dbhandle *db, const char *sql)
{
	char *err = NULL;

	if (sqlite3_exec(db->db, sql, NULL, NULL, &err) != SQLITE_OK)
		abort();
	sqlite3_free(err);
}
FIXTURE uint64_t rows(struct dbhandle *db, const char *table)
{
	char sql[64];

	snprintf(sql, sizeof(sql), "select count(*) from %s", table);
	return dbfile_query_u64(db->db, sql);
}

/* The three counts oans itself reports. */
FIXTURE void digest_of(unsigned char *out, unsigned int n)
{
	for (unsigned int i = 0; i < DIGEST_LEN; i++)
		out[i] = (unsigned char)(n * 7 + i * 31);
}
/* The one row builder. Everything below adds columns to it rather than
 * repeating the build - two near-identical copies differing by an inert line
 * is what this replaced. */
FIXTURE int64_t put_row(struct dbhandle *db, const char *name, uint64_t ino,
		       uint64_t subvol, uint64_t size, unsigned int seq)
{
	_cleanup_(file_cleanup) struct file f = {0};
	int64_t id;

	if (file_set_filename(&f, name))
		abort();
	f.ino = ino;
	f.subvol = subvol;
	f.size = size;
	f.mtime = 1000;
	f.dedupe_seq = seq;
	id = dbfile_store_file_info(db, &f);
	if (id <= 0)
		abort();
	return id;
}
FIXTURE int64_t put_unscanned_file(struct dbhandle *db, const char *name,
				  uint64_t ino, uint64_t subvol)
{
	return put_row(db, name, ino, subvol, 4096, 1);
}

/* ... and the same row finished, the way a completed hash leaves it. */
FIXTURE int64_t put_file(struct dbhandle *db, const char *name, uint64_t ino,
			uint64_t subvol)
{
	int64_t id = put_unscanned_file(db, name, ino, subvol);
	unsigned char digest[DIGEST_LEN];

	digest_of(digest, (unsigned int)ino);
	if (dbfile_update_scanned_file(db, id, digest, 0, 1))
		abort();
	return id;
}

/* Build the map, ask, free - the shape share() and key_of() above have. */
FIXTURE int layout_matches(struct dbhandle *db, int64_t fileid,
			  const struct fm_rec *recs, unsigned int n)
{
	struct fiemap *fm = mkmap(recs, n);
	int ret = dbfile_layout_matches(db, fileid, fm);

	free(fm);
	return ret;
}

/*
 * `files` is unique on the *pair* (ino, subvol), and both halves matter: a
 * subvolume id and an inode number are each 64 bits and two different files can
 * share either one alone. Conflating them is the same trap seen_inode() has, so
 * it is pinned here as a property of the schema.
 *
 * What this deliberately does *not* assert is that a second link destroys the
 * first row. That is true today - storing one is an INSERT OR REPLACE - but it
 * is the hazard, not the contract: the protection is an in-memory seen_inodes
 * set in file_scan.c, pinned end-to-end by tests/integration/test_hardlinks.py.
 * A test demanding the destructive behaviour would go red on the day someone
 * makes the write safe, and read as a regression while the tests that actually
 * guard it stayed green. If that day comes, delete this comment rather than
 * repairing anything.
 */
#define PROP_BLOCK	4096

/*
 * A plausible extent layout: ascending, block-aligned, sometimes with a hole
 * before a record, addresses drawn from a small pool so that two layouts
 * genuinely collide rather than never doing so. NO_PHYS records appear about
 * one map in eight, because "cannot prove it" is a case with its own rules and
 * a generator that never produced one would leave them to the fixed tests.
 */
FIXTURE int64_t prop_donor(struct dbhandle *db, struct prop *p)
{
	char name[64];

	snprintf(name, sizeof(name), "/snap/%u", p->iteration);
	return put_file(db, name, 100000 + p->iteration, 1);
}

/* The fiemap records, as the rows a scan would have stored for them. */
#define PROP_FILES	4
#define PROP_BLOCKS	24

/*
 * Offsets and extent lengths use their own constant rather than the global
 * `blocksize`, which an earlier test sets to 100 and never restores. Nothing
 * here depends on the value, only on it being fixed.
 */
#define PROP_LEN	4096

/* Takes the fileid it will actually have: the tests look filerecs up by id,
 * so a helper that invented its own numbering had to be bypassed by the one
 * property that chooses ids. */
FIXTURE struct filerec *mkfilerec(int64_t fileid)
{
	char name[64];
	struct filerec *f;

	snprintf(name, sizeof(name), "/tree/f%lld", (long long)fileid);
	f = filerec_new(name, fileid, PROP_LEN * (uint64_t)fileid);
	if (!f)
		abort();
	return f;
}

FIXTURE void prop_shuffle_u64(struct prop *p, uint64_t *a, unsigned int n)
{
	for (unsigned int i = n; i > 1; i--) {
		unsigned int j = (unsigned int)prop_below(p, i);
		uint64_t t = a[i - 1];

		a[i - 1] = a[j];
		a[j] = t;
	}
}

FIXTURE int64_t put_dupe(struct dbhandle *db, const char *name, uint64_t ino,
			unsigned int dg, uint64_t size, unsigned int seq,
			unsigned int flags, unsigned int nr_extents)
{
	int64_t id = put_row(db, name, ino, 1, size, seq);
	unsigned char digest[DIGEST_LEN];

	digest_of(digest, dg);
	if (dbfile_update_scanned_file(db, id, digest, flags, nr_extents))
		abort();
	return id;
}

/*
 * The sole group in a results tree. abort()s rather than returning NULL when
 * there is not exactly one: that means the fixture is broken, and a test that
 * quietly took the first of two groups would pass while the loader split them.
 */
FIXTURE struct dupe_extents *only_group(struct results_tree *res)
{
	if (res->num_dupes != 1 || RB_EMPTY_ROOT(&res->root))
		abort();
	return rb_entry(rb_first(&res->root), struct dupe_extents, de_node);
}

/*
 * The group's dedupe target: the loader orders it first, and
 * dedupe_extent_list() always dedupes against list_first_entry().
 *
 * abort()s on an empty member list, because list_first_entry() on one returns
 * a pointer derived from the list head rather than NULL - so a NULL check at
 * the call site would pass while dereferencing nothing.
 */
#define FM_CHUNK	(256 * 1024)

/* A real file to map. fd is -1 when this filesystem has no FIEMAP, which the
 * callers treat as a skip rather than as a failure. */
struct fm_file {
	int fd;
	char path[128];
	uint64_t size;
};

FIXTURE struct fm_file fm_open(const char *name, unsigned int chunks, bool sparse)
{
	struct fm_file f = { .fd = -1 };
	_cleanup_(freep) char *buf = malloc(FM_CHUNK);
	const char *dir = getenv("DUPEREMOVE_TEST_DIR");
	struct fiemap probe = { .fm_length = ~0ULL };
	int fd;

	if (!buf)
		abort();
	memset(buf, 0xa5, FM_CHUNK);
	snprintf(f.path, sizeof(f.path), "%s/oans-fiemap-XXXXXX",
		 dir && *dir ? dir : "/tmp");
	fd = mkstemp(f.path);
	if (fd < 0)
		abort();

	/* Ask before relying on it. tmpfs answers EOPNOTSUPP here, and a bare
	 * assertion failure would say nothing about why. */
	if (ioctl(fd, FS_IOC_FIEMAP, &probe) < 0 &&
	    (errno == EOPNOTSUPP || errno == ENOTTY)) {
		printf("\n[fiemap] skipping %s: %s has no FIEMAP\n", name, f.path);
		close(fd);
		unlink(f.path);
		f.path[0] = '\0';
		return f;			/* fd stays -1 */
	}

	for (unsigned int i = 0; i < chunks; i++) {
		/* A gap of one chunk between each, so a sparse file really has
		 * holes rather than a filesystem's idea of a contiguous run. */
		off_t at = (off_t)i * FM_CHUNK * (sparse ? 2 : 1);

		if (pwrite(fd, buf, FM_CHUNK, at) != FM_CHUNK) {
			unlink(f.path);
			abort();
		}
		f.size = (uint64_t)at + FM_CHUNK;
	}
	/*
	 * Punch the gaps rather than assuming a skipped write leaves one. XFS
	 * reserves post-EOF blocks on a buffered extending write - for a file
	 * this size, about the size of the gap - and reports the reservation as
	 * a DELALLOC record, so the "hole" would come back mapped. Truncating
	 * to the written size drops anything left past the end.
	 */
	if (sparse) {
		for (unsigned int i = 1; i < chunks; i++)
			(void)fallocate(fd, FALLOC_FL_PUNCH_HOLE |
					FALLOC_FL_KEEP_SIZE,
					(off_t)(2 * i - 1) * FM_CHUNK, FM_CHUNK);
	}
	if (ftruncate(fd, (off_t)f.size)) {
		unlink(f.path);
		abort();
	}
	/* Without this the data can still be in delalloc, where fiemap reports
	 * it with no stable physical address - or not at all. */
	if (fsync(fd)) {
		unlink(f.path);
		abort();
	}
	f.fd = fd;
	return f;
}

/*
 * The count pass and the mapping pass agree with each other and with the file.
 *
 * do_fiemap() is two ioctls - count, then map that many - so the interesting
 * failure is them disagreeing: a map sized from a stale count either truncates
 * the extent list or leaves uninitialised records at the end, and every caller
 * walks fm_mapped_extents believing it.
 */

#endif /* OANS_TEST_FIXTURES_H */
