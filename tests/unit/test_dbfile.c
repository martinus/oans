/*
 * The hashfile: storage, change detection, and the dedupe-phase loaders.
 *
 * Compiled as part of tu_dbfile.c, which is where the sources these tests reach
 * into are #included.
 */
static struct dbfile_stats stats_of(struct dbhandle *db)
{
	struct dbfile_stats st = {0};

	if (dbfile_get_stats(db, &st))
		abort();
	return st;
}

/*
 * A checkpoint carries a real running-checksum state: `file_state` is bound as
 * a blob of running_checksum_state_size() bytes, so a NULL there is a
 * constraint failure rather than an empty column. The caller owns the buffers,
 * which is also how dbfile_load_checkpoint() reads one back - one convention,
 * not two.
 *
 * `ext_state` is the half that matters most (#159): a checkpoint at an extent
 * boundary needs none, but btrfs reports a contiguously allocated file as a
 * single fiemap extent however large it is, so the boundary-only first cut
 * never fired. Pass NULL for the boundary case and a buffer for the in-flight
 * one.
 */
static struct scan_checkpoint mkcheckpoint(void *file_state, void *ext_state,
					   uint64_t loff)
{
	size_t len = running_checksum_state_size();
	struct running_checksum *c = start_running_checksum();
	struct scan_checkpoint cp = {
		.loff = loff, .size = loff * 2, .mtime = 1000,
		.ext_loff = 0, .ext_len = 4096,
		.file_state = file_state,
		.ext_state = ext_state,
		.has_ext_state = ext_state != NULL,
	};

	add_to_running_checksum(c, (unsigned char *)"some bytes", 10);
	if (running_checksum_save(c, file_state, len))
		abort();
	finish_running_checksum(c, NULL);
	if (ext_state)
		memcpy(ext_state, file_state, len);
	return cp;
}

/* Store one file row; returns its id. */
/*
 * A row for a file that has been *seen* but not finished - which is what
 * `dbfile_store_file_info` alone produces, because it deliberately writes no
 * digest. The digest is precisely what tells a scanned file from one an
 * interrupted run left partway, so this is the shape the startup prune is
 * about, and the two helpers are split so a test can ask for either.
 */
/* A digest that differs in every byte for each n, so a comparator reading the
 * wrong end of it still sees a difference. */
MU_TEST(test_dbfile_files_are_unique_on_the_inode_pair) {
	_cleanup_(sqlite3_close_cleanup) struct dbhandle *db = memdb();

	put_file(db, "/tree/a", 42, 1);
	mu_check(stats_of(db).num_files == 1);

	/* A different inode in the same subvolume is a different file... */
	put_file(db, "/tree/c", 43, 1);
	mu_check(stats_of(db).num_files == 2);

	/* ...and so is the same inode number in a different subvolume. */
	put_file(db, "/other/a", 42, 2);
	mu_check(stats_of(db).num_files == 3);

}

/*
 * A file's hashes belong to it: dropping the row takes them with it, via the
 * ON DELETE CASCADE. If that FK is ever lost, pruning a deleted file leaves
 * orphan hashes that match nothing and are never collected.
 *
 * Driven through dbfile_prune_missing_files() rather than dbfile_remove_file(),
 * so it exercises the caller the comment is about. That needs no filesystem:
 * the rule is stat-based, and "/tree/a" genuinely does not exist, which is
 * exactly the ENOENT the prune looks for. It is stat-based rather than
 * "delete what this run did not walk" precisely so that scanning a subdirectory
 * cannot wipe rows that are merely out of scope.
 */
MU_TEST(test_dbfile_pruning_a_deleted_file_takes_its_hashes) {
	_cleanup_(sqlite3_close_cleanup) struct dbhandle *db = memdb();
	int64_t id = put_file(db, "/tree/a", 1, 1);
	struct block_csum blocks[2] = { { .loff = 0 }, { .loff = 4096 } };
	struct extent_csum extents[1] = { { .loff = 0, .poff = 8192, .len = 8192 } };

	mu_check(dbfile_store_block_hashes(db, id, 2, blocks) == 0);
	mu_check(dbfile_store_extent_hashes(db, id, 1, extents) == 0);
	mu_check(stats_of(db).num_b_hashes == 2);
	mu_check(stats_of(db).num_e_hashes == 1);

	mu_check(dbfile_prune_missing_files(db, NULL) == 1);
	mu_check(stats_of(db).num_files == 0);
	mu_check(stats_of(db).num_b_hashes == 0);	/* cascaded */
	mu_check(stats_of(db).num_e_hashes == 0);

}

/*
 * #159's resume path. A file interrupted partway keeps its row, its stored
 * hashes and its checkpoint; the run that picks it up moves it into the current
 * dedupe generation with a *targeted* UPDATE. Re-running the ordinary upsert
 * instead would INSERT OR REPLACE the row and cascade the checkpoint and every
 * stored hash away - the file would silently restart from byte zero, which on
 * the 1 TiB file this feature exists for is hours.
 */
MU_TEST(test_dbfile_advancing_the_generation_keeps_hashes_and_checkpoint) {
	_cleanup_(sqlite3_close_cleanup) struct dbhandle *db = memdb();
	int64_t id = put_file(db, "/tree/big", 1, 1);
	struct block_csum blocks[1] = { { .loff = 0 } };
	size_t len = running_checksum_state_size();
	_cleanup_(freep) void *file_state = malloc(len);
	_cleanup_(freep) void *ext_state = malloc(len);
	/* Interrupted mid-extent, which is the case that fires in practice:
	 * btrfs reports a contiguously allocated file as one fiemap extent
	 * however large it is, so a boundary-only checkpoint never happens. */
	struct scan_checkpoint cp = mkcheckpoint(file_state, ext_state, 1u << 30);
	/* load_checkpoint copies into buffers the caller owns; a zeroed struct
	 * would be a write through NULL. */
	_cleanup_(freep) void *got_file = malloc(len);
	_cleanup_(freep) void *got_ext = malloc(len);
	struct scan_checkpoint got = { .file_state = got_file, .ext_state = got_ext };

	mu_check(dbfile_store_block_hashes(db, id, 1, blocks) == 0);
	mu_check(dbfile_store_checkpoint(db, id, &cp) == 0);
	mu_check(rows(db, "blocks") == 1);
	mu_check(rows(db, "scan_checkpoints") == 1);

	mu_check(dbfile_update_dedupe_seq(db, id, 7) == 0);

	/* The generation moved... */
	mu_check(dbfile_query_u64(db->db,
		 "select dedupe_seq from files") == 7);
	/* ...and nothing else did. */
	mu_check(rows(db, "files") == 1);
	mu_check(rows(db, "blocks") == 1);
	mu_check(dbfile_load_checkpoint(db, id, &got));
	mu_check(got.loff == cp.loff && got.ext_len == cp.ext_len);
	mu_check(!memcmp(got.file_state, file_state, len));
	/* The in-flight extent digest rides along, or a resume that lands inside
	 * an extent has to discard the checkpoint and start the file over. */
	mu_check(got.has_ext_state);
	mu_check(!memcmp(got.ext_state, ext_state, len));

}

/*
 * The startup prune drops rows that were never hashed - a digest is what says
 * a file was finished - but it has to spare rows an interrupted run left a
 * checkpoint for. Before #159 it deleted every digest-less row, which is
 * exactly what kept a resumable file from surviving to be resumed.
 */
MU_TEST(test_dbfile_pruning_unscanned_files_spares_checkpointed_ones) {
	_cleanup_(sqlite3_close_cleanup) struct dbhandle *db = memdb();
	_cleanup_(freep) void *state = malloc(running_checksum_state_size());
	/* No ext_state: a checkpoint that happened to land on an extent
	 * boundary, which is the other half of what dbfile_load_checkpoint
	 * validates. */
	struct scan_checkpoint cp = mkcheckpoint(state, NULL, 4096);

	/* A finished file: has a digest. */
	put_file(db, "/tree/done", 1, 1);

	/* One interrupted with a checkpoint, one simply never hashed. Neither
	 * has a digest, which is the only thing the prune looks at. */
	int64_t pid = put_unscanned_file(db, "/tree/partway", 2, 1);

	put_unscanned_file(db, "/tree/abandoned", 3, 1);

	mu_check(dbfile_store_checkpoint(db, pid, &cp) == 0);
	mu_check(stats_of(db).num_files == 3);

	mu_check(dbfile_prune_unscanned_files(db) == 0);

	/* The finished one and the checkpointed one survive; the third goes. */
	mu_check(stats_of(db).num_files == 2);
	mu_check(dbfile_query_u64(db->db,
		 "select count(*) from files where filename = '/tree/partway'") == 1);
	mu_check(dbfile_query_u64(db->db,
		 "select count(*) from files where filename = '/tree/abandoned'") == 0);

}

/*
 * The self-describing hashfile (#182): a 1.7.x file stores
 * opt_skip_readonly_subvols = -1, the old tri-state "auto". That means "the
 * user never asked", so a replay must adopt today's default rather than
 * inheriting a dead one - only an explicit 1 survives. Getting this backwards
 * silently skips every snapshot on a scheduled run.
 */
MU_TEST(test_dbfile_scan_config_round_trips_and_coerces_the_old_auto) {
	_cleanup_(sqlite3_close_cleanup) struct dbhandle *db = memdb();
	char *roots[] = { (char *)"/data", (char *)"/srv" };
	char *excludes[] = { (char *)"*.iso" };
	struct scan_config out = {
		.run_dedupe = 1, .recurse = 1, .skip_zeroes = 1,
		.skip_readonly_subvols = 1, .only_whole_files = 1,
		.do_block_hash = 1, .dedupe_same_file = 1,
		.min_filesize = 1024, .max_filesize = 1u << 20,
		.roots = roots, .nroots = 2,
		.excludes = excludes, .nexcludes = 1,
	};
	struct scan_config in = {0};

	/* 0 means "nothing stored", which is what a fresh hashfile answers and
	 * what tells a replay from a first run. */
	mu_check(dbfile_load_scan_config(db, &in) == 0);
	mu_check(in.nroots == 0);

	mu_check(dbfile_store_scan_config(db, &out) == 0);
	mu_check(dbfile_load_scan_config(db, &in) == 1);

	/*
	 * Every option, not a representative few: a replay adopts whatever this
	 * does not read, so an option the loader drops silently reverts to its
	 * default on a scheduled run and the hashfile stops describing what
	 * produced it. All 1 here, so a dropped key reads as 0.
	 */
	mu_check(in.run_dedupe == 1 && in.recurse == 1 && in.do_block_hash == 1);
	mu_check(in.skip_zeroes == 1 && in.only_whole_files == 1);
	mu_check(in.dedupe_same_file == 1);
	mu_check(in.min_filesize == 1024 && in.max_filesize == (1u << 20));
	mu_check(in.skip_readonly_subvols == 1);	/* an explicit 1 survives */
	mu_check(in.nroots == 2 && in.nexcludes == 1);
	mu_check(!strcmp(in.roots[0], "/data") && !strcmp(in.roots[1], "/srv"));
	mu_check(!strcmp(in.excludes[0], "*.iso"));
	scan_config_free(&in);

	/* What a 1.7.x hashfile holds. */
	exec(db, "update config set keyval = -1 "
		 "where keyname = 'opt_skip_readonly_subvols'");
	memset(&in, 0, sizeof(in));
	mu_check(dbfile_load_scan_config(db, &in) == 1);
	mu_check(in.skip_readonly_subvols == 0);	/* coerced to the new default */
	scan_config_free(&in);

	/* ...and the mirror image, so that all-1 above cannot be a constant. */
	memset(&out, 0, sizeof(out));
	out.roots = roots;
	out.nroots = 2;
	out.min_filesize = 4096;
	mu_check(dbfile_store_scan_config(db, &out) == 0);
	memset(&in, 0, sizeof(in));
	mu_check(dbfile_load_scan_config(db, &in) == 1);
	mu_check(!in.run_dedupe && !in.recurse && !in.do_block_hash);
	mu_check(!in.skip_zeroes && !in.only_whole_files && !in.dedupe_same_file);
	scan_config_free(&in);

	/*
	 * A key that is simply not there. Sizes are the two that a hashfile
	 * written by an older oans can lack, and the default has to be "no
	 * limit" rather than whatever the caller's struct happened to hold -
	 * a stale non-zero max_filesize skips every large file for good.
	 */
	exec(db, "delete from config where keyname in "
		 "('opt_min_filesize', 'opt_max_filesize')");
	memset(&in, 0x5a, sizeof(in));
	mu_check(dbfile_load_scan_config(db, &in) == 1);
	mu_check(in.min_filesize == 0 && in.max_filesize == 0);
	mu_check(in.run_dedupe == 0 && in.nroots == 2);
	scan_config_free(&in);

	/*
	 * Only the additive one missing, which is what a hashfile written
	 * before --max-filesize existed actually looks like. The two limits
	 * are read through one scratch variable, so a reset dropped between
	 * them silently gives max_filesize the value of min - and every file
	 * above the lower limit stops being scanned, on a run that otherwise
	 * looks identical.
	 */
	mu_check(dbfile_store_scan_config(db, &out) == 0);
	exec(db, "delete from config where keyname = 'opt_max_filesize'");
	memset(&in, 0x5a, sizeof(in));
	mu_check(dbfile_load_scan_config(db, &in) == 1);
	mu_check(in.min_filesize == 4096);
	mu_check(in.max_filesize == 0);
	scan_config_free(&in);
}

/*
 * Run history, which `--history` and `--json` are read straight out of. The
 * lifetime totals accumulate across runs while the error buckets report only
 * the *last* one - a monitoring consumer alarms on the second and trends on
 * the first, so conflating them either hides a fault or re-raises a fixed one
 * forever.
 */
MU_TEST(test_dbfile_run_history_totals_accumulate_but_skips_are_the_last_run) {
	_cleanup_(sqlite3_close_cleanup) struct dbhandle *db = memdb();
	struct run_record first = {
		.ts = 1000, .duration_ms = 500, .files_scanned = 10,
		.reclaimed = 4096, .groups = 2, .deduped = 1,
		.skip_permission = 3, .skip_unreadable = 1,
	};
	/* Five adjacent columns read back by index, so five distinct values:
	 * two buckets that share one cannot tell a swapped pair apart, and
	 * a bucket left at 0 cannot tell a dropped read from a real zero. */
	struct run_record second = {
		.ts = 2000, .duration_ms = 700, .files_scanned = 5,
		.reclaimed = 8192, .groups = 1, .deduped = 1,
		.skip_permission = 11, .skip_unreadable = 22,
		.skip_path_too_long = 33, .skip_unsupported_fs = 44,
		.readonly_subvols = 55,
	};
	struct run_record third = {
		.ts = 3000, .duration_ms = 100, .files_scanned = 1,
	};
	/* Deliberately not zeroed: the summary clears what it does not fill,
	 * so a caller's stack garbage cannot be read back as a lifetime total. */
	struct run_summary s;

	memset(&s, 0x5a, sizeof(s));

	/*
	 * A hashfile nobody has run against yet. Every field must be zero,
	 * which is what says the summary clears the caller's struct rather
	 * than filling in only what its two queries return - with no rows,
	 * they return nothing at all. (It has to be asked here: memdb()
	 * handles all share one in-memory database, so there is no such thing
	 * as a second, empty one within a test.)
	 */
	mu_check(dbfile_get_run_summary(db, &s) == 0);
	mu_check(s.runs == 0 && s.total_reclaimed == 0 && s.total_files == 0);
	mu_check(s.first_ts == 0 && s.last_ts == 0 && s.total_skip_errors == 0);
	mu_check(s.last_skip_permission == 0 && s.last_skip_unreadable == 0);
	mu_check(s.last_skip_path_too_long == 0);
	mu_check(s.last_skip_unsupported_fs == 0 && s.last_readonly_subvols == 0);

	memset(&s, 0x5a, sizeof(s));
	mu_check(dbfile_record_run(db, &first) == 0);
	mu_check(dbfile_record_run(db, &second) == 0);
	mu_check(dbfile_get_run_summary(db, &s) == 0);

	mu_check(s.runs == 2);
	mu_check(s.total_reclaimed == 4096 + 8192);	/* lifetime */
	mu_check(s.total_files == 15);
	mu_check(s.first_ts == 1000 && s.last_ts == 2000);

	/* Each bucket, from the most recent run only. */
	mu_check(s.last_skip_permission == 11);
	mu_check(s.last_skip_unreadable == 22);
	mu_check(s.last_skip_path_too_long == 33);
	mu_check(s.last_skip_unsupported_fs == 44);
	mu_check(s.last_readonly_subvols == 55);
	/* ...while the lifetime figure sums both runs. readonly_subvols is not
	 * an error, so it is not in the total. */
	mu_check(s.total_skip_errors == 3 + 1 + 11 + 22 + 33 + 44);

	/*
	 * A later clean run clears the alarm. A lifetime total only ever
	 * grows, so it cannot tell "last night lost a subtree" from "one run
	 * did, months ago" - which is the whole reason these are read
	 * separately from the sums above.
	 */
	mu_check(dbfile_record_run(db, &third) == 0);
	mu_check(dbfile_get_run_summary(db, &s) == 0);
	mu_check(s.last_skip_permission == 0 && s.last_skip_unreadable == 0);
	mu_check(s.last_skip_path_too_long == 0);
	mu_check(s.last_skip_unsupported_fs == 0 && s.last_readonly_subvols == 0);
	mu_check(s.total_skip_errors == 3 + 1 + 11 + 22 + 33 + 44);

}

/*
 * #206's second bar. A layout key is a 128-bit digest, so it cannot be the
 * whole test: before copying a donor's hashes the scan re-checks the donor's
 * stored (loff, poff, len) triples one at a time. A miss costs one redundant
 * hash; a false hit stores a digest of bytes the file never had, and nothing
 * downstream could tell that from a file with no duplicate.
 */
MU_TEST(test_dbfile_layout_matches_compares_every_record) {
	_cleanup_(sqlite3_close_cleanup) struct dbhandle *db = memdb();
	int64_t donor = put_file(db, "/snap/a", 1, 1);
	struct extent_csum stored[2] = {
		{ .loff = 0,    .poff = 4096,  .len = 8192 },
		{ .loff = 8192, .poff = 65536, .len = 4096 },
	};
	struct fm_rec same[]  = { {0, 4096, 8192, 0}, {8192, 65536, 4096, 0} };
	struct fm_rec moved[] = { {0, 4096, 8192, 0}, {8192, 69632, 4096, 0} };
	struct fm_rec shortr[] = { {0, 4096, 8192, 0} };
	struct fm_rec longer[] = { {0, 4096, 8192, 0}, {8192, 65536, 4096, 0},
				   {12288, 73728, 4096, 0} };

	mu_check(dbfile_store_extent_hashes(db, donor, 2, stored) == 0);

	mu_check(layout_matches(db, donor, same, ARRAY_SIZE(same)) == 1);
	/* One address moved: not the same storage. */
	mu_check(layout_matches(db, donor, moved, ARRAY_SIZE(moved)) == 0);
	/* The candidate has fewer records than the donor, and more. Both
	 * directions, because a prefix match is still a miss. */
	mu_check(layout_matches(db, donor, shortr, ARRAY_SIZE(shortr)) == 0);
	mu_check(layout_matches(db, donor, longer, ARRAY_SIZE(longer)) == 0);

	/*
	 * A donor whose rows are gone - an aborted batch, a hardlink cascade -
	 * verifies as a miss by construction rather than needing a guard, which
	 * is why the in-memory map can hold only a key and a file id.
	 */
	int64_t empty = put_file(db, "/snap/b", 2, 1);

	mu_check(layout_matches(db, empty, same, ARRAY_SIZE(same)) == 0);

	/*
	 * And the case that needs saying separately: *both* sides empty. The
	 * record counts then agree at zero, so a check that only compared them
	 * would call two files with no extents a match and copy one's digest
	 * onto the other. "No records" is not evidence of identical storage,
	 * it is the absence of evidence.
	 */
	mu_check(layout_matches(db, empty, same, 0) == 0);

}

/*
 * The copy itself (#206): the destination ends up with the donor's extent rows,
 * block rows and digest. Everything downstream reads those, so a copy that
 * dropped one of the three would leave a file that looks scanned and dedupes
 * against nothing.
 */
MU_TEST(test_dbfile_copying_a_donor_brings_all_three) {
	_cleanup_(sqlite3_close_cleanup) struct dbhandle *db = memdb();
	int64_t donor = put_file(db, "/snap/a", 1, 1);
	int64_t dst;
	struct block_csum blocks[2] = { { .loff = 0 }, { .loff = 4096 } };
	struct extent_csum extents[1] = { { .loff = 0, .poff = 4096, .len = 8192 } };

	mu_check(dbfile_store_block_hashes(db, donor, 2, blocks) == 0);
	mu_check(dbfile_store_extent_hashes(db, donor, 1, extents) == 0);

	/* The destination starts with no digest, the way a file does before it
	 * has been hashed. */
	dst = put_unscanned_file(db, "/snap/b", 2, 1);

	mu_check(dbfile_copy_scanned_file(db, dst, donor, 0) == 0);

	mu_check(stats_of(db).num_b_hashes == 4);	/* two each */
	mu_check(stats_of(db).num_e_hashes == 2);
	/* And the destination now has a digest, so it reads as scanned. */
	mu_check(dbfile_query_u64(db->db,
		 "select count(*) from files where digest is not null") == 2);

}

/*
 * Every field a stored row carries, read back through the change-detection
 * path the scan itself uses. The original test asserted only that a row
 * appeared, which leaves each `sqlite3_bind_*` free to write the right value
 * into the wrong column - a sweep found the bind indices in
 * dbfile_store_file_info() entirely unguarded. Distinct values throughout, so
 * a swapped pair cannot look correct.
 */
MU_TEST(test_dbfile_a_stored_file_round_trips_every_field) {
	_cleanup_(sqlite3_close_cleanup) struct dbhandle *db = memdb();
	_cleanup_(file_cleanup) struct file f = {0};
	_cleanup_(file_cleanup) struct file got = {0};
	unsigned char digest[DIGEST_LEN];
	int64_t id;

	if (file_set_filename(&f, "/tree/distinctive-name"))
		abort();
	f.ino = 111; f.subvol = 222; f.size = 333; f.mtime = 444;
	f.dedupe_seq = 5;
	id = dbfile_store_file_info(db, &f);
	mu_check(id > 0);

	/*
	 * Before the digest lands the row exists but is not a finished file -
	 * which is the whole distinction an interrupted run turns on, and the
	 * only thing that tells the two apart.
	 */
	mu_check(dbfile_describe_file(db, 111, 222, &got) == 0);
	mu_check(got.id == id && !got.digest_valid);
	file_cleanup(&got);
	memset(&got, 0, sizeof(got));

	memset(digest, 0xab, DIGEST_LEN);
	mu_check(dbfile_update_scanned_file(db, id, digest, 6, 7) == 0);

	/* Looked up by (ino, subvol), which is how the scan finds it again. */
	mu_check(dbfile_describe_file(db, 111, 222, &got) == 0);
	mu_check(got.id == id);
	mu_check(got.size == 333);
	mu_check(got.mtime == 444);
	mu_check(got.filename && !strcmp(got.filename, "/tree/distinctive-name"));
	mu_check(got.digest_valid);	/* it has been hashed all the way through */

	/* The columns describe_file does not return, and the ones only
	 * update_scanned_file writes. */
	mu_check(dbfile_query_u64(db->db, "select ino from files") == 111);
	mu_check(dbfile_query_u64(db->db, "select subvol from files") == 222);
	mu_check(dbfile_query_u64(db->db, "select dedupe_seq from files") == 5);
	mu_check(dbfile_query_u64(db->db, "select flags from files") == 6);
	mu_check(dbfile_query_u64(db->db, "select nr_extents from files") == 7);
	mu_check(dbfile_query_u64(db->db,
		 "select count(*) from files "
		 "where hex(digest) = 'ABABABABABABABABABABABABABABABAB'") == 1);

	/* A file nothing knows about leaves the caller's struct alone rather
	 * than reporting someone else's row. */
	_cleanup_(file_cleanup) struct file absent = {0};

	mu_check(dbfile_describe_file(db, 999, 999, &absent) == 0);
	mu_check(absent.id == 0 && absent.size == 0 && absent.filename == NULL);
}

/*
 * Hashes carry the offsets that say where they came from, and the loaders join
 * on them. Asserting only the row *count* - which is what the first cut did -
 * leaves every bind index free: a block hash stored at the wrong loff still
 * counts as one row, and dedupe then compares the wrong parts of two files.
 */
MU_TEST(test_dbfile_hashes_round_trip_their_offsets) {
	_cleanup_(sqlite3_close_cleanup) struct dbhandle *db = memdb();
	int64_t id = put_unscanned_file(db, "/tree/a", 1, 1);
	struct block_csum blocks[2] = { { .loff = 4096 }, { .loff = 8192 } };
	struct extent_csum extents[2] = {
		{ .loff = 0,    .poff = 65536, .len = 4096 },
		{ .loff = 4096, .poff = 69632, .len = 8192 },
	};

	memset(blocks[0].digest, 0x11, DIGEST_LEN);
	memset(blocks[1].digest, 0x22, DIGEST_LEN);
	memset(extents[0].digest, 0x33, DIGEST_LEN);
	memset(extents[1].digest, 0x44, DIGEST_LEN);

	mu_check(dbfile_store_block_hashes(db, id, 2, blocks) == 0);
	mu_check(dbfile_store_extent_hashes(db, id, 2, extents) == 0);

	mu_check(dbfile_query_u64(db->db,
		 "select loff from blocks order by loff limit 1") == 4096);
	mu_check(dbfile_query_u64(db->db,
		 "select loff from blocks order by loff desc limit 1") == 8192);
	mu_check(dbfile_query_u64(db->db,
		 "select count(*) from blocks where fileid = (select id from files)") == 2);
	/* Paired with its offset, so a digest bound into the wrong slot shows. */
	mu_check(dbfile_query_u64(db->db,
		 "select count(*) from blocks where loff = 4096 and "
		 "hex(digest) = '11111111111111111111111111111111'") == 1);
	mu_check(dbfile_query_u64(db->db,
		 "select count(*) from blocks where loff = 8192 and "
		 "hex(digest) = '22222222222222222222222222222222'") == 1);

	/* Each extent's three numbers, together: a swapped poff/len pair keeps
	 * both the row count and either column's set of values intact. */
	mu_check(dbfile_query_u64(db->db,
		 "select count(*) from extents where loff = 0 and poff = 65536 "
		 "and len = 4096") == 1);
	mu_check(dbfile_query_u64(db->db,
		 "select count(*) from extents where loff = 4096 and poff = 69632 "
		 "and len = 8192") == 1);
	mu_check(dbfile_query_u64(db->db,
		 "select count(*) from extents where loff = 0 and "
		 "hex(digest) = '33333333333333333333333333333333'") == 1);
	mu_check(dbfile_query_u64(db->db,
		 "select count(*) from extents where loff = 4096 and "
		 "hex(digest) = '44444444444444444444444444444444'") == 1);

	/*
	 * A zero-length extent is skipped rather than stored: it names no
	 * bytes, so a later join against it would match a range that does not
	 * exist.
	 */
	struct extent_csum empty[2] = {
		{ .loff = 65536, .poff = 1, .len = 0 },
		{ .loff = 69632, .poff = 2, .len = 1 },
	};

	mu_check(dbfile_store_extent_hashes(db, id, 2, empty) == 0);
	mu_check(dbfile_query_u64(db->db,
		 "select count(*) from extents where len = 0") == 0);
	mu_check(dbfile_query_u64(db->db,
		 "select count(*) from extents where len = 1") == 1);
}

/*
 * The checkpoint reader declines everything it cannot vouch for. Reporting
 * success for a checkpoint that is absent or the wrong size hands the scan a
 * resume position built from an uninitialised buffer, and the digest that comes
 * out describes bytes no file ever held.
 */
MU_TEST(test_dbfile_load_checkpoint_declines_what_it_cannot_vouch_for) {
	_cleanup_(sqlite3_close_cleanup) struct dbhandle *db = memdb();
	int64_t id = put_unscanned_file(db, "/tree/big", 1, 1);
	size_t len = running_checksum_state_size();
	_cleanup_(freep) void *state = malloc(len);
	_cleanup_(freep) void *got_file = malloc(len);
	_cleanup_(freep) void *got_ext = malloc(len);
	struct scan_checkpoint cp = mkcheckpoint(state, NULL, 4096);
	struct scan_checkpoint got = { .file_state = got_file, .ext_state = got_ext };

	/* Distinct throughout: these are five adjacent columns read by index,
	 * so two that share a value cannot tell a swapped pair apart. */
	cp.size = 123456;
	cp.mtime = 777;
	cp.ext_loff = 2048;
	cp.ext_len = 999;

	/* Nothing stored yet: not an error, but not a checkpoint either. */
	mu_check(!dbfile_load_checkpoint(db, id, &got));

	mu_check(dbfile_store_checkpoint(db, id, &cp) == 0);
	mu_check(dbfile_load_checkpoint(db, id, &got));
	mu_check(got.loff == 4096 && got.size == 123456 && got.mtime == 777);
	mu_check(got.ext_loff == 2048 && got.ext_len == 999);
	mu_check(!got.has_ext_state);	/* none was stored, so none comes back */
	/* The saved hash state itself, which is the point of the whole row. */
	mu_check(!memcmp(got_file, state, len));

	/* A file that has one tells nothing about a file that does not. */
	int64_t other = put_unscanned_file(db, "/tree/other", 2, 1);

	mu_check(!dbfile_load_checkpoint(db, other, &got));

	/* A state blob of the wrong length is refused rather than copied out
	 * of - the length is the only thing standing between a truncated row
	 * and a memcpy past the caller's buffer. */
	exec(db, "update scan_checkpoints set state = x'0011'");
	mu_check(!dbfile_load_checkpoint(db, id, &got));

	/* And once removed it is gone, so a finished file cannot resume. */
	mu_check(dbfile_store_checkpoint(db, id, &cp) == 0);
	mu_check(dbfile_load_checkpoint(db, id, &got));
	mu_check(dbfile_remove_checkpoint(db, id) == 0);
	mu_check(!dbfile_load_checkpoint(db, id, &got));
}

/* Which id the seen-oracle below claims the walk confirmed on disk. */
static int64_t seen_oracle_id;

static bool claims_seen(int64_t id)
{
	return id == seen_oracle_id;
}

/*
 * The prune's actual contract, which the first cut only tested from one side:
 * it removes rows whose path is *gone* and keeps everything else. Testing only
 * the removal half leaves "delete what this run did not walk" passing, and that
 * is the mistake the rule exists to prevent - it would wipe a shared hashfile
 * the moment anyone scanned a subdirectory.
 *
 * Needs a real file, but any filesystem will do: the rule is a stat(), not a
 * dedupe.
 */
MU_TEST(test_dbfile_pruning_keeps_what_still_exists) {
	_cleanup_(sqlite3_close_cleanup) struct dbhandle *db = memdb();
	char present[] = "/tmp/oans-prune-XXXXXX";
	int fd = mkstemp(present);

	if (fd < 0)
		abort();
	close(fd);

	put_file(db, present, 1, 1);
	put_file(db, "/tmp/oans-prune-definitely-not-here", 2, 1);
	mu_check(stats_of(db).num_files == 2);

	/* One gone, one still there. */
	mu_check(dbfile_prune_missing_files(db, NULL) == 1);
	mu_check(stats_of(db).num_files == 1);
	mu_check(dbfile_query_u64(db->db, "select ino from files") == 1);

	/*
	 * The seen-oracle is the scan's "I confirmed this one on disk" answer,
	 * and it must short-circuit the stat entirely - that is the whole point
	 * of it, since the common case is that nothing was deleted. Claim the
	 * missing file was seen and it survives.
	 */
	int64_t ghost = put_file(db, "/tmp/oans-prune-also-not-here", 3, 1);

	seen_oracle_id = ghost;
	mu_check(dbfile_prune_missing_files(db, claims_seen) == 0);
	mu_check(stats_of(db).num_files == 2);

	/* Withdraw the claim and it goes. */
	seen_oracle_id = 0;
	mu_check(dbfile_prune_missing_files(db, claims_seen) == 1);
	mu_check(stats_of(db).num_files == 1);

	/*
	 * ENOTDIR, the other way a path stops naming a file: a component of it
	 * is now a regular file rather than a directory. It is a separate
	 * errno from ENOENT and the check names both, so a test that only
	 * deletes files leaves half of that condition free.
	 */
	char under[sizeof(present) + 8];

	snprintf(under, sizeof(under), "%s/child", present);
	put_file(db, under, 4, 1);
	mu_check(stats_of(db).num_files == 2);
	mu_check(dbfile_prune_missing_files(db, NULL) == 1);
	mu_check(dbfile_query_u64(db->db, "select ino from files") == 1);

	/*
	 * A stat that failed for any *other* reason is not "gone", and the row
	 * has to survive: EACCES on a directory whose permissions changed, or
	 * EIO on a disk going bad, would otherwise delete the hashes for files
	 * that are still there - so the run that was meant to notice the disk
	 * is failing rebuilds the cache instead. A symlink loop stands in for
	 * that class, because it is the only one of them a test can produce
	 * without being root or breaking hardware.
	 */
	char loop[] = "/tmp/oans-prune-loop-XXXXXX";

	if (!mkdtemp(loop))
		abort();
	rmdir(loop);
	if (symlink(loop, loop))
		abort();
	put_file(db, loop, 5, 1);
	mu_check(stats_of(db).num_files == 2);
	mu_check(dbfile_prune_missing_files(db, NULL) == 0);
	mu_check(stats_of(db).num_files == 2);
	unlink(loop);

	unlink(present);
}

/*
 * More missing files than the collector's initial capacity, so the array has to
 * grow. 512 is where it starts; a growth that loses or duplicates ids deletes
 * the wrong rows, and at 8 files nothing would ever notice.
 */
MU_TEST(test_dbfile_pruning_grows_past_its_initial_capacity) {
	_cleanup_(sqlite3_close_cleanup) struct dbhandle *db = memdb();
	const unsigned int n = 600;
	char name[64];

	mu_check(dbfile_begin_trans(db->db) == 0);
	for (unsigned int i = 0; i < n; i++) {
		snprintf(name, sizeof(name), "/tmp/oans-absent-%u", i);
		put_file(db, name, 1000 + i, 1);
	}
	mu_check(dbfile_commit_trans(db->db) == 0);
	mu_check(stats_of(db).num_files == n);

	mu_check(dbfile_prune_missing_files(db, NULL) == (int64_t)n);
	mu_check(stats_of(db).num_files == 0);
}

/*
 * ---------------------------------------------------------------------------
 * Property-based tests. See src/proptest.h for the harness and for why the
 * seed is fixed; `OANS_PROPTEST_SEED=random ./test` goes looking for more.
 *
 * Everything below states a relationship that has to hold for *every* input,
 * rather than an input and its answer. The tests above are not redundant with
 * these and are not replaced by them: a table of cases says what a function is
 * for and reads as documentation, while a property says what must never
 * happen and reaches inputs nobody sits down and writes.
 * ---------------------------------------------------------------------------
 */

/*
 * A name as hostile as anything a scanned tree can contain: mostly ordinary
 * bytes, with C0 controls, DEL and the two-byte UTF-8 C1 encodings salted in
 * at a rate no uniform draw would reach. The 0xc2 lead byte is emitted on its
 * own often enough to land immediately before the terminator, which is the
 * case ctrl_seq_len() has to read one byte past to decide.
 */
MU_TEST(test_prop_stored_extents_come_back_where_they_were_put) {
	declare_prop(p, 300);
	_cleanup_(sqlite3_close_cleanup) struct dbhandle *db = memdb();

	while (prop_next(&p)) {
		struct extent_csum ext[6];
		unsigned int n = (unsigned int)prop_range(&p, 0, ARRAY_SIZE(ext));
		uint64_t loff = 0, expect = 0;
		int64_t id = prop_donor(db, &p);
		char sql[256];

		for (unsigned int i = 0; i < n; i++) {
			ext[i].loff = loff;
			ext[i].poff = PROP_BLOCK * prop_range(&p, 100, 108);
			/* Empty about one record in five, so the skip is
			 * reached first, last and alone across the run. */
			ext[i].len = prop_chance(&p, 5)
				   ? 0 : PROP_BLOCK * prop_range(&p, 1, 3);
			memset(ext[i].digest, (int)prop_u64(&p), DIGEST_LEN);
			loff += PROP_BLOCK * prop_range(&p, 1, 4);
			if (ext[i].len)
				expect++;
		}

		if (dbfile_store_extent_hashes(db, id, n, ext))
			abort();

		for (unsigned int i = 0; i < n; i++) {
			if (!ext[i].len)
				continue;
			snprintf(sql, sizeof(sql),
				 "select count(*) from extents where "
				 "fileid = %lld and loff = %llu and "
				 "poff = %llu and len = %llu",
				 (long long)id, (unsigned long long)ext[i].loff,
				 (unsigned long long)ext[i].poff,
				 (unsigned long long)ext[i].len);
			prop_check(&p, dbfile_query_u64(db->db, sql) == 1);
		}

		snprintf(sql, sizeof(sql),
			 "select count(*) from extents where fileid = %lld",
			 (long long)id);
		prop_check(&p, dbfile_query_u64(db->db, sql) == expect);
	}
}

/*
 * ---------------------------------------------------------------------------
 * The in-memory dedupe model: filerecs, the hash tree, the results tree
 *
 * All three are pure memory - no filesystem, no btrfs, no scan - and all three
 * were at a 0% mutation kill rate before this: 502 mutants, nothing caught.
 * They are also the only callers of the vendored kernel rbtree, so generated
 * insert and *erase* orders here are what exercise that copy's rebalancing;
 * inserting and removing in one ascending sweep would only ever walk a spine.
 * ---------------------------------------------------------------------------
 */

static const char *target_of(struct dupe_extents *d)
{
	if (list_empty(&d->de_extents))
		abort();
	return list_first_entry(&d->de_extents, struct extent, e_list)
		->e_file->filename;
}

/*
 * A whole-file duplicate group loads every member of the window at poff 0.
 *
 * That zero is not incidental. GET_DUPLICATE_FILES has no fiemap to draw on,
 * so whole-file members carry no physical offset - which is exactly why they
 * skip clean_deduped() and why `--dedupe-options=only_whole_files` converged
 * while the extent path did not (#186). A loader that invented a poff here
 * would put them back on the path that failed to converge.
 */
MU_TEST(test_whole_file_dupes_load_at_poff_zero) {
	_cleanup_(sqlite3_close_cleanup) struct dbhandle *db = memdb();
	struct results_tree res;
	struct dupe_extents *d;
	struct extent *e;
	unsigned int n = 0;

	free_all_filerecs();
	init_results_tree(&res);

	/* Three copies of one 8 KiB file, all scanned in this window. The
	 * tidiest is deliberately not the first inserted, so "target first" is
	 * a claim about the election rather than about insertion order. */
	put_dupe(db, "/tree/a", 1, 7, 8192, 1, 0, 4);
	put_dupe(db, "/tree/b", 2, 7, 8192, 1, 0, 1);
	put_dupe(db, "/tree/c", 3, 7, 8192, 1, 0, 6);
	/* A file of the same size but a different digest is not a duplicate. */
	put_dupe(db, "/tree/d", 4, 8, 8192, 1, 0, 1);
	/* ...and neither is one of the same digest at a different size. */
	put_dupe(db, "/tree/e", 5, 7, 4096, 1, 0, 1);

	mu_check(dbfile_load_same_files(db, &res, 0, 1) == 0);

	mu_check(res.num_dupes == 1);
	d = only_group(&res);
	mu_check(d->de_num_dupes == 3);
	mu_check(d->de_len == 8192);
	mu_assert_string_eq("/tree/b", target_of(d));	/* ordered first */

	list_for_each_entry(e, &d->de_extents, e_list) {
		mu_check(e->e_loff == 0);
		mu_check(e->e_poff == 0);	/* no fiemap here, and #186 */
		n++;
	}
	mu_check(n == 3);

	free_results_tree(&res);
	free_all_filerecs();
}

/* Load one window over three same-size copies with the given per-file
 * (flags, nr_extents), and report which one the query elected. Names are
 * assigned in the order given, so a caller can put a low id anywhere. */
static const char *elected_target(const char *const names[3],
				  const unsigned int flags[3],
				  const unsigned int nr_extents[3])
{
	_cleanup_(sqlite3_close_cleanup) struct dbhandle *db = memdb();
	static char winner[64];
	struct results_tree res;

	free_all_filerecs();
	init_results_tree(&res);
	for (unsigned int i = 0; i < 3; i++)
		put_dupe(db, names[i], i + 1, 7, 8192, 1, flags[i],
			 nr_extents[i]);
	if (dbfile_load_same_files(db, &res, 0, 1))
		abort();
	snprintf(winner, sizeof(winner), "%s", target_of(only_group(&res)));
	free_results_tree(&res);
	free_all_filerecs();
	return winner;
}

/*
 * Which member becomes the target, one ranking level per case.
 *
 * A read-only member wins outright: the kernel will not write into one, so it
 * has to be the source rather than a destination (#172). Below that the
 * least-fragmented member wins, because every copy inherits the target's
 * fragmentation. Both are read from columns fixed at scan time rather than
 * probed live, so every generation window reaches the same answer (#197).
 */
MU_TEST(test_the_whole_file_target_prefers_readonly_then_fewest_extents) {
	static const char *const abc[3] = { "/tree/a", "/tree/b", "/tree/c" };
	/* Ids ascend with position, so putting z first gives it the lowest. */
	static const char *const zyx[3] = { "/tree/z", "/tree/y", "/tree/x" };
	static const unsigned int none[3] = { 0, 0, 0 };

	/* Fewest extents wins, and the winner is neither first nor lowest id. */
	{
		static const unsigned int nr[3] = { 9, 2, 5 };

		mu_assert_string_eq("/tree/b", elected_target(abc, none, nr));
	}

	/* A read-only member wins even when it is both later and worse. */
	{
		static const unsigned int ro[3] = { 0, 0, FILE_RO_SUBVOL };
		static const unsigned int nr[3] = { 1, 3, 9 };

		mu_assert_string_eq("/tree/c", elected_target(abc, ro, nr));
	}

	/*
	 * Everything else equal, the lowest id breaks the tie - and the names
	 * descend while the ids ascend, so an ordering that fell back to the
	 * filename, or to no tie-break at all, would answer differently.
	 */
	{
		static const unsigned int nr[3] = { 4, 4, 4 };

		mu_assert_string_eq("/tree/z", elected_target(zyx, none, nr));
	}
}

/*
 * #197 itself: two overlapping generation windows must elect the *same*
 * target, and the election ranges over every member of the group rather than
 * the window's.
 *
 * The fixture is built so that weaker elections give different answers. The
 * winner (`old2`) is neither the lowest id nor a member of either window, so
 * an election reduced to min(id) - roughly the pre-#197 behaviour - answers
 * `old1`, and a window-local one answers a member of the window. A fixture
 * whose winner happened to be lowest-id *and* tidiest *and* oldest would pass
 * against all three, which is what the first draft of this test did.
 */
MU_TEST(test_every_window_elects_the_same_whole_file_target) {
	_cleanup_(sqlite3_close_cleanup) struct dbhandle *db = memdb();
	struct results_tree first, second;
	const char *t1, *t2;

	free_all_filerecs();

	/* id 1 is old and ragged; id 2 is old and tidiest - the winner. */
	put_dupe(db, "/tree/old1", 1, 7, 8192, 1, 0, 6);
	put_dupe(db, "/tree/old2", 2, 7, 8192, 1, 0, 1);
	put_dupe(db, "/tree/mid", 3, 7, 8192, 2, 0, 5);
	put_dupe(db, "/tree/new", 4, 7, 8192, 3, 0, 4);

	init_results_tree(&first);
	mu_check(dbfile_load_same_files(db, &first, 1, 2) == 0);
	t1 = target_of(only_group(&first));

	init_results_tree(&second);
	mu_check(dbfile_load_same_files(db, &second, 2, 3) == 0);
	t2 = target_of(only_group(&second));

	mu_assert_string_eq("/tree/old2", t1);
	mu_assert_string_eq("/tree/old2", t2);

	/* Each window loads its own new member plus that target, and nothing
	 * else: two members, not four. */
	mu_check(only_group(&first)->de_num_dupes == 2);
	mu_check(only_group(&second)->de_num_dupes == 2);

	/*
	 * The loader marks the group anchored, from the query's is_target
	 * column. Nothing reads the flag today (#237), which is precisely why
	 * it is asserted here: without this the one C statement in
	 * dbfile_load_same_files() that exists for #197 can be deleted with
	 * every other test still green.
	 */
	mu_check(only_group(&first)->de_anchored);
	mu_check(only_group(&second)->de_anchored);

	free_results_tree(&first);
	free_results_tree(&second);
	free_all_filerecs();
}

/*
 * An inlined file is not a group member, not a target, and never becomes a
 * filerec at all. oans stores no extents for one and the kernel will not
 * deduplicate it, so a group formed around one is work that can only fail.
 */
MU_TEST(test_an_inlined_file_is_never_loaded_as_a_duplicate) {
	_cleanup_(sqlite3_close_cleanup) struct dbhandle *db = memdb();
	struct results_tree res;
	struct dupe_extents *d;
	struct extent *e;
	unsigned char solo[DIGEST_LEN];
	int64_t inlined;

	free_all_filerecs();
	init_results_tree(&res);
	digest_of(solo, 9);

	/* Two real copies and one inlined, all the same digest and size. */
	put_dupe(db, "/tree/a", 1, 7, 8192, 1, 0, 1);
	put_dupe(db, "/tree/b", 2, 7, 8192, 1, 0, 1);
	inlined = put_dupe(db, "/tree/inline", 3, 7, 8192, 1, FILE_INLINED, 0);

	/*
	 * And a pair that is a pair *only* because of the inlined file. The
	 * group must not exist at all: excluding the inlined file from the
	 * membership but not from the count leaves a group of one real copy,
	 * which has nothing to be deduplicated against and whose work figure
	 * is zero. A fixture with two real copies cannot see that, because the
	 * group qualifies on them alone.
	 */
	put_dupe(db, "/tree/solo", 4, 9, 4096, 1, 0, 1);
	put_dupe(db, "/tree/solo-inl", 5, 9, 4096, 1, FILE_INLINED, 0);

	mu_check(dbfile_load_same_files(db, &res, 0, 1) == 0);

	mu_check(res.num_dupes == 1);		/* the digest-7 group, only */
	mu_check(find_dupe_extents(&res, solo, 4096) == NULL);

	d = only_group(&res);
	mu_check(d->de_num_dupes == 2);
	list_for_each_entry(e, &d->de_extents, e_list)
		mu_check(strcmp(e->e_file->filename, "/tree/inline") != 0);

	/* Not merely absent from the group - never constructed. The loader
	 * creates a filerec per row it accepts, so this is an assertion about
	 * code that ran rather than about rows the query withheld. */
	mu_check(filerec_find(inlined) == NULL);

	free_results_tree(&res);
	free_all_filerecs();
}

/*
 * dbfile_load_one_filerec() answers "no such file" with success and a NULL
 * out-parameter, not an error. The dedupe phase calls it for ids it read from
 * rows that may since have been pruned, so a missing file is ordinary; a
 * loader returning an error there would abort a whole batch over a file
 * somebody deleted mid-run.
 */
MU_TEST(test_loading_one_filerec_treats_a_missing_id_as_success) {
	_cleanup_(sqlite3_close_cleanup) struct dbhandle *db = memdb();
	struct filerec *f = NULL;
	int64_t id;

	free_all_filerecs();
	id = put_dupe(db, "/tree/only", 1, 7, 12288, 1, 0, 1);

	mu_check(dbfile_load_one_filerec(db, id, &f) == 0);
	mu_check(f != NULL);
	mu_assert_string_eq("/tree/only", f->filename);
	mu_check(f->fileid == id);
	mu_check(f->size == 12288);		/* the row's size, not a default */

	/* Absent: success, and the caller's pointer cleared rather than left
	 * holding whatever it had. */
	f = (struct filerec *)(uintptr_t)0x1;
	mu_check(dbfile_load_one_filerec(db, id + 999, &f) == 0);
	mu_check(f == NULL);

	free_all_filerecs();
}

/*
 * Block hashes load into a hash tree, and come out of it sorted.
 *
 * The loader ends with sort_file_hash_heads() because find_dupes walks each
 * file's blocks under a hash expecting increasing offsets. GET_DUPLICATE_BLOCKS
 * orders only by (dedupe_seq > ?1), fileid, and add_file_hash_head() appends in
 * arrival order - so rows genuinely arrive jumbled and this is the one place
 * that ordering is established for the dedupe phase.
 */
MU_TEST(test_block_hashes_load_into_the_tree_in_offset_order) {
	_cleanup_(sqlite3_close_cleanup) struct dbhandle *db = memdb();
	struct hash_tree tree;
	unsigned char dg[DIGEST_LEN];
	struct block_csum blocks[3];
	struct dupe_blocks_list *dl;
	struct rb_node *n;
	unsigned int heads = 0;
	int64_t a, b;

	free_all_filerecs();
	init_hash_tree(&tree);
	digest_of(dg, 3);

	a = put_dupe(db, "/tree/a", 1, 7, 12288, 1, 0, 1);
	b = put_dupe(db, "/tree/b", 2, 8, 12288, 1, 0, 1);

	/* Deliberately not ascending. */
	blocks[0].loff = 8192;
	blocks[1].loff = 0;
	blocks[2].loff = 4096;
	for (unsigned int i = 0; i < 3; i++)
		memcpy(blocks[i].digest, dg, DIGEST_LEN);
	mu_check(dbfile_store_block_hashes(db, a, 3, blocks) == 0);
	mu_check(dbfile_store_block_hashes(db, b, 3, blocks) == 0);

	mu_check(dbfile_load_block_hashes(db, &tree, 0, 1) == 0);

	mu_check(tree.num_blocks == 6);
	mu_check(tree.num_hashes == 1);
	dl = find_block_list(&tree, dg);
	mu_check(dl != NULL);
	mu_check(dl->dl_num_elem == 6);

	/* Both files present, each holding exactly 0, 4096, 8192 in order. */
	for (n = rb_first(&dl->dl_files_root); n; n = rb_next(n)) {
		struct file_hash_head *h =
			rb_entry(n, struct file_hash_head, h_node);
		struct file_block *blk;
		unsigned int i = 0;

		heads++;
		list_for_each_entry(blk, &h->h_blocks, b_head_list) {
			mu_check(blk->b_loff == i * 4096);
			i++;
		}
		mu_check(i == 3);
	}
	mu_check(heads == 2);

	free_hash_tree(&tree);
	free_all_filerecs();
}

/*
 * Extent hashes load as a group per (digest, length), carrying the physical
 * offset the scan recorded - which is what the extent path needs and the
 * whole-file path deliberately lacks.
 *
 * A group needs two members: one extent with a digest nothing else shares is
 * not a duplicate of anything, and loading it would put a group into the tree
 * that the worker can only throw away.
 */
MU_TEST(test_extent_hashes_load_as_groups_carrying_their_offsets) {
	_cleanup_(sqlite3_close_cleanup) struct dbhandle *db = memdb();
	struct results_tree res;
	struct dupe_extents *d;
	struct extent *e;
	unsigned char shared[DIGEST_LEN], lonely[DIGEST_LEN];
	struct extent_csum ea[2], eb[1];
	int64_t a, b;
	unsigned int seen = 0;

	free_all_filerecs();
	init_results_tree(&res);
	digest_of(shared, 4);
	digest_of(lonely, 5);

	a = put_dupe(db, "/tree/a", 1, 7, 65536, 1, 0, 2);
	b = put_dupe(db, "/tree/b", 2, 8, 65536, 1, 0, 2);

	/* One extent shared between the files, one only file a has. */
	ea[0].loff = 0;
	ea[0].poff = 4096;
	ea[0].len = 4096;
	memcpy(ea[0].digest, shared, DIGEST_LEN);
	ea[1].loff = 4096;
	ea[1].poff = 8192;
	ea[1].len = 4096;
	memcpy(ea[1].digest, lonely, DIGEST_LEN);
	eb[0].loff = 16384;
	eb[0].poff = 65536;
	eb[0].len = 4096;
	memcpy(eb[0].digest, shared, DIGEST_LEN);

	mu_check(dbfile_store_extent_hashes(db, a, 2, ea) == 0);
	mu_check(dbfile_store_extent_hashes(db, b, 1, eb) == 0);

	mu_check(dbfile_load_extent_hashes(db, &res, 0, 1) == 0);

	/* Only the shared digest makes a group. */
	mu_check(res.num_extents == 2);
	d = only_group(&res);
	mu_check(d->de_len == 4096);
	mu_check(!memcmp(d->de_hash, shared, DIGEST_LEN));

	/*
	 * Never anchored: only the whole-file loader elects a target, because
	 * only there do the members have differing layouts to rank. The pair
	 * with the assertion in the #197 test above is what pins that
	 * asymmetry rather than each half separately looking arbitrary.
	 */
	mu_check(!d->de_anchored);

	/* Each member keeps the physical offset its row recorded - the extent
	 * path reads it to decide what is already shared. */
	list_for_each_entry(e, &d->de_extents, e_list) {
		if (e->e_loff == 0)
			mu_check(e->e_poff == 4096);
		else if (e->e_loff == 16384)
			mu_check(e->e_poff == 65536);
		else
			mu_fail("an extent at an offset nobody stored");
		seen++;
	}
	mu_check(seen == 2);

	free_results_tree(&res);
	free_all_filerecs();
}

/*
 * The extents of one file that nothing else in the hashfile shares.
 *
 * `--dedupe-options=partial` asks this for each file, then searches those
 * extents block by block for matches the extent-level pass could not see. So
 * an extent wrongly included is work the search can only waste, and one
 * wrongly excluded is a duplicate nobody ever finds - and neither shows up
 * anywhere, since both produce a run that exits 0 having deduplicated slightly
 * less than it could.
 *
 * The uniqueness test is over the *whole* extents table, not the file: an
 * extent shared with any other file, including one outside this run's windows,
 * is not a candidate.
 */
MU_TEST(test_nondupe_extents_are_the_ones_nothing_else_shares) {
	_cleanup_(sqlite3_close_cleanup) struct dbhandle *db = memdb();
	_cleanup_(freep) struct file_extent *got = NULL;
	struct filerec *f;
	unsigned char uniq[DIGEST_LEN], shared[DIGEST_LEN];
	struct extent_csum mine[3], theirs[1];
	unsigned int n = 99;
	int64_t a, b;

	free_all_filerecs();
	a = put_dupe(db, "/tree/a", 1, 7, 65536, 1, 0, 3);
	b = put_dupe(db, "/tree/b", 2, 8, 65536, 1, 0, 1);
	f = filerec_new("/tree/a", a, 65536);
	if (!f)
		abort();
	digest_of(uniq, 11);
	digest_of(shared, 12);

	/* Two of a's extents are unique; the middle one is also in b. */
	mine[0].loff = 0;      mine[0].poff = 4096;  mine[0].len = 4096;
	memcpy(mine[0].digest, uniq, DIGEST_LEN);
	mine[1].loff = 4096;   mine[1].poff = 8192;  mine[1].len = 4096;
	memcpy(mine[1].digest, shared, DIGEST_LEN);
	mine[2].loff = 8192;   mine[2].poff = 16384; mine[2].len = 8192;
	digest_of(mine[2].digest, 13);
	theirs[0].loff = 0;    theirs[0].poff = 32768; theirs[0].len = 4096;
	memcpy(theirs[0].digest, shared, DIGEST_LEN);

	mu_check(dbfile_store_extent_hashes(db, a, 3, mine) == 0);
	mu_check(dbfile_store_extent_hashes(db, b, 1, theirs) == 0);

	mu_check(dbfile_load_nondupe_file_extents(db, f, &got, &n) == 0);
	mu_check(n == 2);		/* the shared one is not a candidate */
	mu_check(got != NULL);

	/* All three columns, paired - a loader reading len where poff is
	 * returns the right number of extents describing the wrong bytes. */
	mu_check(got[0].loff == 0 && got[0].poff == 4096 && got[0].len == 4096);
	mu_check(got[1].loff == 8192 && got[1].poff == 16384 && got[1].len == 8192);

	free_all_filerecs();
}

/*
 * More extents than the array starts with, so the geometric growth runs.
 *
 * It begins at 16 and doubles, and the count is the only thing that says how
 * much of the buffer is live - so a growth that loses the tail, or an index
 * that runs past it, is invisible below 17 extents. Deliberately just over two
 * doublings.
 */
MU_TEST(test_nondupe_extents_grow_past_the_initial_capacity) {
	_cleanup_(sqlite3_close_cleanup) struct dbhandle *db = memdb();
	_cleanup_(freep) struct file_extent *got = NULL;
	struct filerec *f;
	struct extent_csum ext[40];
	unsigned int n = 0;
	int64_t a;

	free_all_filerecs();
	a = put_dupe(db, "/tree/a", 1, 7, 1u << 20, 1, 0, 40);
	f = filerec_new("/tree/a", a, 1u << 20);
	if (!f)
		abort();

	for (unsigned int i = 0; i < ARRAY_SIZE(ext); i++) {
		ext[i].loff = i * 4096;
		ext[i].poff = (i + 100) * 4096;
		ext[i].len = 4096;
		digest_of(ext[i].digest, 100 + i);	/* all distinct */
	}
	mu_check(dbfile_store_extent_hashes(db, a, ARRAY_SIZE(ext), ext) == 0);

	mu_check(dbfile_load_nondupe_file_extents(db, f, &got, &n) == 0);
	mu_check(n == ARRAY_SIZE(ext));

	/* Every one of them, at its own offsets: a lost tail would still leave
	 * a plausible array of a plausible length. */
	for (unsigned int i = 0; i < n; i++) {
		mu_check(got[i].loff == i * 4096);
		mu_check(got[i].poff == (i + 100) * 4096);
		mu_check(got[i].len == 4096);
	}

	free_all_filerecs();
}

/*
 * A file whose every extent is shared has no candidates, and says so with a
 * count of zero rather than by leaving the caller's count untouched -
 * find_dupes tests `!num_extents` before it looks at the array.
 */
MU_TEST(test_a_file_with_nothing_unique_yields_no_nondupe_extents) {
	_cleanup_(sqlite3_close_cleanup) struct dbhandle *db = memdb();
	_cleanup_(freep) struct file_extent *got = NULL;
	struct filerec *f;
	unsigned char dg[DIGEST_LEN];
	struct extent_csum mine[1], theirs[1];
	unsigned int n = 99;			/* must be overwritten */
	int64_t a, b;

	free_all_filerecs();
	a = put_dupe(db, "/tree/a", 1, 7, 4096, 1, 0, 1);
	b = put_dupe(db, "/tree/b", 2, 8, 4096, 1, 0, 1);
	f = filerec_new("/tree/a", a, 4096);
	if (!f)
		abort();
	digest_of(dg, 21);

	mine[0].loff = 0;   mine[0].poff = 4096;  mine[0].len = 4096;
	memcpy(mine[0].digest, dg, DIGEST_LEN);
	theirs[0].loff = 0; theirs[0].poff = 8192; theirs[0].len = 4096;
	memcpy(theirs[0].digest, dg, DIGEST_LEN);
	mu_check(dbfile_store_extent_hashes(db, a, 1, mine) == 0);
	mu_check(dbfile_store_extent_hashes(db, b, 1, theirs) == 0);

	mu_check(dbfile_load_nondupe_file_extents(db, f, &got, &n) == 0);
	mu_check(n == 0);
	mu_check(got == NULL);		/* nothing allocated, nothing to free */

	free_all_filerecs();
}

/*
 * ---------------------------------------------------------------------------
 * The extent search (find_dupes.c)
 *
 * What `--dedupe-options=partial` runs: for a file extent nothing else shares,
 * walk two files' block trees in lockstep looking for runs of blocks that
 * carry the same digest *and* sit contiguously in both files. It was 237 of
 * 247 mutants surviving - 4% killed - and the whole of it is reachable from
 * here, because compare_extents() and record_match() are static in a file
 * tests.c #includes, so they can be called directly rather than through the
 * thread pool and the database that normally drive them.
 *
 * A run found too long is a dedupe request the kernel byte-verify rejects;
 * one found too short is a duplicate nobody ever finds. Neither says anything
 * at the time.
 * ---------------------------------------------------------------------------
 */

/*
 * What every `perror_sqlite` + `goto out` pair is for.
 *
 * dbfile.c has 82 of them and until now no unit test reached one: they guard a
 * bind or a step failing, which a healthy in-memory database never does. The
 * residue was triaged as needing a fault-injection seam - and the seam is
 * sqlite's own `query_only`, which needs no change to the product.
 *
 * The invariant is the one that matters for a cache nothing downstream
 * validates. A hashfile is not checked by anything that reads it, so a write
 * that failed and *said so* costs one re-hash, while a write that failed and
 * returned success produces a run that exits 0, looks correct, and has a hole
 * in it. Every one of these asserts the first and would fail on the second.
 */
MU_TEST(test_a_refused_write_is_reported_rather_than_swallowed) {
	_cleanup_(sqlite3_close_cleanup) struct dbhandle *db = memdb();
	int64_t fileid;
	struct file f;
	unsigned char digest[DIGEST_LEN];
	struct block_csum block;
	struct scan_checkpoint cp;
	struct run_record run;

	/* A real row first, on a healthy connection, so the ids below are not
	 * themselves the reason a write fails. */
	fileid = put_file(db, "/refused", 4001, 1);
	mu_check(fileid > 0);

	db_refuse_writes(db);

	/* Each of these binds and steps a write. Under query_only the step
	 * answers SQLITE_READONLY, which is precisely the branch the pairs
	 * guard - and the answer must not be "fine". */
	memset(&f, 0, sizeof(f));
	f.filename = "/also-refused";
	f.ino = 4002;
	f.subvol = 1;
	f.size = 4096;
	f.mtime = 222;
	mu_assert(dbfile_store_file_info(db, &f) <= 0,
		  "a refused insert reported a rowid");

	memset(digest, 0xcd, sizeof(digest));
	mu_assert(dbfile_update_scanned_file(db, fileid, digest, 0, 1) != 0,
		  "a refused digest update reported success");

	memset(&block, 0, sizeof(block));
	block.loff = 0;
	memcpy(block.digest, digest, DIGEST_LEN);
	mu_assert(dbfile_store_block_hashes(db, fileid, 1, &block) != 0,
		  "a refused block-hash write reported success");

	memset(&cp, 0, sizeof(cp));
	cp.loff = 4096;
	cp.mtime = 222;
	cp.size = 4096;
	mu_assert(dbfile_store_checkpoint(db, fileid, &cp) != 0,
		  "a refused checkpoint reported success - the one write whose\n"
		  "  whole purpose is that a later run can trust it");

	memset(&run, 0, sizeof(run));
	run.files_scanned = 1;
	mu_assert(dbfile_record_run(db, &run) != 0,
		  "a refused run-history append reported success");
}

/*
 * The read side still works while writes are refused.
 *
 * Without this the test above is satisfied by a connection that is simply
 * broken - every call failing for any reason would pass it. This is what makes
 * "the write was refused" a different statement from "nothing works".
 */
MU_TEST(test_reads_still_answer_while_writes_are_refused) {
	_cleanup_(sqlite3_close_cleanup) struct dbhandle *db = memdb();
	int64_t fileid = put_file(db, "/still-readable", 4010, 1);
	struct file out;

	mu_check(fileid > 0);
	db_refuse_writes(db);

	memset(&out, 0, sizeof(out));
	mu_check(dbfile_describe_file(db, 4010, 1, &out) == 0);
	mu_assert(out.id == fileid,
		  "the read path stopped answering, so the test above proves nothing");
	/* describe_file strdup()s the name into `out`; dbfile.h says free it
	 * through file_cleanup(). LeakSanitizer caught this missing. */
	file_cleanup(&out);
}
