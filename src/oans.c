/*
 * oans.c
 *
 * Copyright (C) 2013 SUSE.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * Authors: Mark Fasheh <mfasheh@suse.de>
 */

#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>
#include <errno.h>
#include <string.h>
#include <getopt.h>
#include <inttypes.h>
#include <sys/stat.h>

#include <glib.h>

#include "list.h"
#include "csum.h"
#include "filerec.h"
#include "hash-tree.h"
#include "results-tree.h"
#include "dedupe.h"
#include "util.h"
#include "btrfs-util.h"
#include "dbfile.h"
#include "memstats.h"
#include "debug.h"
#include "progress.h"
#include "file_scan.h"
#include "find_dupes.h"
#include "run_dedupe.h"

#include "opt.h"

unsigned int blocksize = DEFAULT_BLOCKSIZE;

static int stdin_filelist = 0;
static unsigned int list_only_opt = 0;
static unsigned int rm_only_opt = 0;
static unsigned int stats_only_opt = 0;
static int opt_no_color = 0;
struct dbfile_config dbfile_cfg;

static enum {
	H_READ,
	H_WRITE,
	H_UPDATE,
} use_hashfile = H_UPDATE;

/* Upper bound for the auto-detected worker thread count (overridable). */
#define DEFAULT_MAX_AUTO_THREADS	8

static void print_file(char *filename, char *ino, char *subvol)
{
	if (verbose)
		printf("%s\t%s\t%s\n", filename, ino, subvol);
	else
		printf("%s\n", filename);
}

static uint64_t stats_u64(sqlite3 *db, const char *sql)
{
	_cleanup_(sqlite3_stmt_cleanup) sqlite3_stmt *stmt = NULL;
	uint64_t v = 0;

	if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK &&
	    sqlite3_step(stmt) == SQLITE_ROW)
		v = sqlite3_column_int64(stmt, 0);
	return v;
}

/* Human duration for the timing lines: ms under a second, else seconds. */
static const char *fmt_dur(char *buf, size_t n, double s)
{
	if (s < 1.0)
		snprintf(buf, n, "%.0f ms", s * 1000.0);
	else
		snprintf(buf, n, "%.2f s", s);
	return buf;
}

/*
 * Print a human-readable report about a hashfile (folds in the old hashstats
 * tool): identity, contents, how much whole-file duplication it records, and
 * how long the load and the duplicate-analysis queries took on this hashfile.
 */
static int print_hashfile_stats(char *filename)
{
	_cleanup_(sqlite3_close_cleanup) struct dbhandle *db = NULL;
	struct dbfile_config cfg;
	struct dbfile_stats st = {0};
	struct stat sb;
	struct topgrp { uint64_t size, count, waste; char *path; } top[10];
	char uuid_str[37] = "";
	char htype[9] = "", b1[16], b2[16];
	int k, ntop = 0, i;
	uint64_t files, hashed, unhashed, logical, app_id;
	uint64_t groups, dupfiles, reclaim;
	uint64_t page_size, page_count, freelist;
	double t0, t_load, t_analysis;
	sqlite3 *sq;

	/* --- load: open the hashfile and read its header + counts --- */
	t0 = g_get_monotonic_time() / 1e6;
	db = dbfile_open_handle(filename);
	if (!db) {
		eprintf("Error: Could not open \"%s\"\n", filename);
		return -1;
	}
	sq = db->db;
	if (dbfile_get_config(sq, &cfg))
		return -1;
	dbfile_get_stats(db, &st);
	uuid_unparse(cfg.fs_uuid, uuid_str);
	memcpy(htype, cfg.hash_type, 8);
	for (k = 7; k >= 0 && (htype[k] == ' ' || htype[k] == '\0'); k--)
		htype[k] = '\0';
	app_id = stats_u64(sq, "PRAGMA application_id");
	page_size = stats_u64(sq, "PRAGMA page_size");
	page_count = stats_u64(sq, "PRAGMA page_count");
	freelist = stats_u64(sq, "PRAGMA freelist_count");
	files    = stats_u64(sq, "select count(*) from files");
	hashed   = stats_u64(sq, "select count(*) from files where digest is not null");
	unhashed = files - hashed;
	logical  = stats_u64(sq, "select ifnull(sum(size),0) from files");
	t_load = g_get_monotonic_time() / 1e6 - t0;

	/* --- duplicate analysis: the group-by over (digest, size) --- */
	t0 = g_get_monotonic_time() / 1e6;
	groups   = stats_u64(sq, "select count(*) from (select 1 from files "
		"where digest is not null group by digest, size having count(*) > 1)");
	dupfiles = stats_u64(sq, "select ifnull(sum(c),0) from (select count(*) c "
		"from files where digest is not null group by digest, size having c > 1)");
	reclaim  = stats_u64(sq, "select ifnull(sum((c-1)*size),0) from (select size, "
		"count(*) c from files where digest is not null group by digest, size having c > 1)");
	{
		_cleanup_(sqlite3_stmt_cleanup) sqlite3_stmt *s = NULL;

		/*
		 * The example is the group's shortest path (ties broken
		 * alphabetically): usually the canonical copy, and the least
		 * nested / most readable. min() over a fixed-width length prefix
		 * picks it; substr() then strips the 10-char prefix back off.
		 */
		if (sqlite3_prepare_v2(sq,
			"select size, count(*) c, "
			"substr(min(printf('%010d', length(filename)) || filename), 11), "
			"(count(*)-1)*size w "
			"from files where digest is not null "
			"group by digest, size having c > 1 "
			"order by w desc, size desc limit 10", -1, &s, NULL) == SQLITE_OK) {
			while (ntop < 10 && sqlite3_step(s) == SQLITE_ROW) {
				const char *fn = (const char *)sqlite3_column_text(s, 2);

				top[ntop].size = sqlite3_column_int64(s, 0);
				top[ntop].count = sqlite3_column_int64(s, 1);
				top[ntop].path = fn ? strdup(fn) : NULL;
				top[ntop].waste = sqlite3_column_int64(s, 3);
				ntop++;
			}
		}
	}
	t_analysis = g_get_monotonic_time() / 1e6 - t0;

	/* --- report --- */
	printf("%s%soans hashfile%s  %s", col_bold, col_blue, col_reset, filename);
	if (stat(filename, &sb) == 0)
		printf("  %s(%s on disk)%s", col_dim, human_size(sb.st_size), col_reset);
	printf("\n");
	printf("  %sformat%s          %d.%d%s\n", col_dim, col_reset, cfg.major,
	       cfg.minor, app_id == OANS_APP_ID ? "  (oans)" : "");
	printf("  %shashing%s         %s, %s blocks\n", col_dim, col_reset,
	       htype, human_size(cfg.blocksize));
	printf("  %sfilesystem%s      %s\n", col_dim, col_reset, uuid_str);
	if (page_count > 0)
		printf("  %sfree space%s      %s   %s(%.1f%% of the file, reclaimed by a VACUUM)%s\n",
		       col_dim, col_reset, human_size(freelist * page_size),
		       col_dim, 100.0 * freelist / page_count, col_reset);

	printf("\n%s%sfiles%s\n", col_bold, col_blue, col_reset);
	printf("  %stracked%s         %"PRIu64"\n", col_dim, col_reset, files);
	printf("  %shashed%s          %"PRIu64"\n", col_dim, col_reset, hashed);
	if (unhashed)
		printf("  %sunread%s          %"PRIu64"   %s(unique size, whole-file mode)%s\n",
		       col_dim, col_reset, unhashed, col_dim, col_reset);
	printf("  %sextent hashes%s   %"PRIu64"\n", col_dim, col_reset, st.num_e_hashes);
	printf("  %sblock hashes%s    %"PRIu64"\n", col_dim, col_reset, st.num_b_hashes);
	printf("  %slogical data%s    %s\n", col_dim, col_reset, human_size(logical));

	printf("\n%s%swhole-file duplicates%s\n", col_bold, col_blue, col_reset);
	printf("  %sgroups%s          %"PRIu64"\n", col_dim, col_reset, groups);
	printf("  %sfiles in groups%s %"PRIu64"\n", col_dim, col_reset, dupfiles);
	printf("  %sreclaimable%s     %s%s%s\n", col_dim, col_reset, col_green,
	       human_size(reclaim), col_reset);
	if (ntop)
		printf("  %stop groups%s      %ssize x copies · reclaimable · example%s\n",
		       col_dim, col_reset, col_dim, col_reset);
	for (i = 0; i < ntop; i++) {
		char szb[48], wb[32];

		snprintf(szb, sizeof(szb), "%s x %"PRIu64, human_size(top[i].size),
			 top[i].count);
		snprintf(wb, sizeof(wb), "%s", human_size(top[i].waste));
		printf("    %-18s %s%10s%s  %s\n", szb, col_dim, wb, col_reset,
		       top[i].path ? top[i].path : "");
		free(top[i].path);
	}

	printf("\n%s%stiming%s\n", col_bold, col_blue, col_reset);
	printf("  %sload%s            %s\n", col_dim, col_reset,
	       fmt_dur(b1, sizeof(b1), t_load));
	printf("  %sdup analysis%s    %s   %s(the group-by over %"PRIu64" files)%s\n",
	       col_dim, col_reset, fmt_dur(b2, sizeof(b2), t_analysis),
	       col_dim, hashed, col_reset);
	return 0;
}

static int list_db_files(char *filename)
{
	int ret;

	_cleanup_(sqlite3_close_cleanup) struct dbhandle *db = dbfile_open_handle(filename);
	if (!db) {
		eprintf("Error: Could not open \"%s\"\n", filename);
		return -1;
	}

	ret = dbfile_iter_files(db, &print_file);
	return ret;
}

static void rm_db_files_from_stdin(struct dbhandle *db)
{
	_cleanup_(freep) char *path = NULL;
	size_t pathlen = 0;
	ssize_t readlen;

	while ((readlen = getline(&path, &pathlen, stdin)) != -1) {
		if (readlen == 0)
			continue;

		if (readlen > 0 && path[readlen - 1] == '\n') {
			path[--readlen] = '\0';
		}

		if (readlen > PATH_MAX - 1) {
			eprintf("Path max exceeded: %s\n", path);
			continue;
		}

		dbfile_remove_file(db, path);
	}
}

static int rm_db_files(int numfiles, char **files)
{
	int i, ret;
	_cleanup_(sqlite3_close_cleanup) struct dbhandle *db = dbfile_open_handle(options.hashfile);
	if (!db) {
		eprintf("Error: Could not open \"%s\"\n", options.hashfile);
		return -1;
	}

	for (i = 0; i < numfiles; i++) {
		const char *name = files[i];

		if (strlen(name) == 1 && name[0] == '-')
			rm_db_files_from_stdin(db);

		ret = dbfile_remove_file(db, name);
		if (ret == 0)
			vprintf("Removed \"%s\" from hashfile.\n", name);

		if (ret)
			printf("ret ?\n");
	}
	return 0;
}

static void print_version(void)
{
	char *s = NULL;
#ifdef	DEBUG_BUILD
	s = " (debug build)";
#endif
	printf("oans %s%s\n", VERSTRING, s ? s : "");
}

/* adapted from ocfs2-tools */
static int parse_dedupe_opts(const char *raw_opts)
{
	_cleanup_(freep) char *opts;
	char *token, *next, *p, *arg;
	int print_usage = false;
	int invert, ret = 0;

	opts = strdup(raw_opts);

	for (token = opts; token && *token; token = next) {
		p = strchr(token, ',');
		next = NULL;
		invert = 0;

		if (p) {
			*p = '\0';
			next = p + 1;
		}

		arg = strstr(token, "no");
		if (arg == token) {
			invert = 1;
			token += strlen("no");
		}

		if (strcmp(token, "same") == 0) {
			options.dedupe_same_file = !invert;
		} else if (strcmp(token, "partial") == 0) {
			options.do_block_hash = !invert;
		} else if (strcmp(token, "only_whole_files") == 0) {
			options.only_whole_files = !invert;
		} else {
			print_usage = true;
			break;
		}
	}

	if (print_usage) {
		eprintf("Bad dedupe options specified. Valid dedupe "
			"options are:\n"
			"\t[no]same\n"
			"\t[no]only_whole_files\n"
			"\t[no]partial\n");
		ret = EINVAL;
	}

	return ret;
}

enum {
	DEBUG_OPTION = CHAR_MAX + 1,
	HELP_OPTION,
	VERSION_OPTION,
	WRITE_HASHES_OPTION,
	READ_HASHES_OPTION,
	HASHFILE_OPTION,
	IO_THREADS_OPTION,
	CPU_THREADS_OPTION,
	SKIP_ZEROES_OPTION,
	FDUPES_OPTION,
	DEDUPE_OPTS_OPTION,
	QUIET_OPTION,
	EXCLUDE_OPTION,
	BATCH_SIZE_OPTION,
	NO_COLOR_OPTION,
	MIN_FILESIZE_OPTION,
	STATS_OPTION,
};

static int process_fdupes(void)
{
	int ret = 0;
	_cleanup_(freep) char *path = NULL;
	size_t pathlen = 0;
	ssize_t readlen;

	while ((readlen = getline(&path, &pathlen, stdin)) != -1) {
		if (readlen == 0)
			continue;

		if (readlen == 1 && path[0] == '\n') {
			ret = fdupes_dedupe();
			if (ret)
				return ret;
			free_all_filerecs();
			continue;
		}

		if (readlen > 0 && path[readlen - 1] == '\n') {
			path[--readlen] = '\0';
		}

		if (readlen > PATH_MAX - 1) {
			eprintf("Path max exceeded: %s\n", path);
			continue;
		}

		add_file_fdupes(path);
	}

	return 0;
}

static int add_files_from_stdin(struct dbhandle *db)
{
	_cleanup_(freep) char *path = NULL;
	size_t pathlen = 0;
	ssize_t readlen;

	while ((readlen = getline(&path, &pathlen, stdin)) != -1) {
		if (readlen == 0)
			continue;

		if (readlen > 0 && path[readlen - 1] == '\n') {
			path[--readlen] = '\0';
		}

		if (readlen > PATH_MAX - 1) {
			eprintf("Path max exceeded: %s\n", path);
			continue;
		}

		if (scan_file(path, db)) {
			eprintf("Error: cannot add %s into the lookup list\n",
				path);
			return 1;
		}
	}

	return 0;
}

static int scan_files_from_cmdline(int numfiles, char **files, struct dbhandle *db)
{
	int i;

	for (i = 0; i < numfiles; i++) {
		char *name = files[i];

		if (scan_file(name, db)) {
			eprintf("Error: cannot scan %s\n", name);
			return 1;
		}
	}

	return 0;
}

static void help(void)
{
	execlp("man", "man", "8", "oans", NULL);
}

/*
 * Ok this is doing more than just parsing options.
 */
static int parse_options(int argc, char **argv, int *filelist_idx)
{
	int c, numfiles;
	bool read_hashes = false;
	bool write_hashes = false;
	bool update_hashes = false;

	static struct option long_ops[] = {
		{ "debug", 0, NULL, DEBUG_OPTION },
		{ "help", 0, NULL, HELP_OPTION },
		{ "version", 0, NULL, VERSION_OPTION },
		{ "write-hashes", 1, NULL, WRITE_HASHES_OPTION },
		{ "read-hashes", 1, NULL, READ_HASHES_OPTION },
		{ "hashfile", 1, NULL, HASHFILE_OPTION },
		{ "io-threads", 1, NULL, IO_THREADS_OPTION },
		{ "hash-threads", 1, NULL, IO_THREADS_OPTION },
		{ "cpu-threads", 1, NULL, CPU_THREADS_OPTION },
		{ "skip-zeroes", 0, NULL, SKIP_ZEROES_OPTION },
		{ "fdupes", 0, NULL, FDUPES_OPTION },
		{ "dedupe-options=", 1, NULL, DEDUPE_OPTS_OPTION },
		{ "quiet", 0, NULL, QUIET_OPTION },
		{ "exclude", 1, NULL, EXCLUDE_OPTION },
		{ "batchsize", 1, NULL, BATCH_SIZE_OPTION },
		{ "no-color", 0, NULL, NO_COLOR_OPTION },
		{ "min-filesize", 1, NULL, MIN_FILESIZE_OPTION },
		{ "stats", 0, NULL, STATS_OPTION },
		{ NULL, 0, NULL, 0}
	};

	if (argc < 2) {
		help(); /* Never returns */
	}

	while ((c = getopt_long(argc, argv, "b:vdDrh?LRqB:m:", long_ops, NULL))
	       != -1) {
		switch (c) {
		case 'b':
			blocksize = parse_size(optarg);
			if (blocksize < MIN_BLOCKSIZE ||
			    blocksize > MAX_BLOCKSIZE){
				eprintf("Error: Blocksize is bounded by %u and %u, %u found\n",
					MIN_BLOCKSIZE, MAX_BLOCKSIZE, blocksize);
				return EINVAL;
			}
			break;
		case 'd':
		case 'D':
			options.run_dedupe += 1;
			break;
		case 'r':
			options.recurse_dirs = true;
			break;
		case VERSION_OPTION:
			print_version();
			exit(0);
		case DEBUG_OPTION:
			debug = 1;
			verbose = 1;
			break;
		case 'v':
			verbose = 1;
			break;
		case 'h':
			human_readable = 1;
			break;
		case WRITE_HASHES_OPTION:
			write_hashes = true;
			options.hashfile = strdup(optarg);
			break;
		case READ_HASHES_OPTION:
			read_hashes = true;
			options.hashfile = strdup(optarg);
			break;
		case HASHFILE_OPTION:
			update_hashes = true;
			options.hashfile = strdup(optarg);
			break;
		case IO_THREADS_OPTION:
			options.io_threads = strtoul(optarg, NULL, 10);
			if (!options.io_threads){
				eprintf("Error: --io-threads must be "
					"an integer, %s found\n", optarg);
				return EINVAL;
			}
			break;
		case CPU_THREADS_OPTION:
			options.cpu_threads = strtoul(optarg, NULL, 10);
			if (!options.cpu_threads){
				eprintf("Error: --cpu-threads must be "
					"an integer, %s found\n", optarg);
				return EINVAL;
			}
			break;
		case SKIP_ZEROES_OPTION:
			options.skip_zeroes = true;
			break;
		case FDUPES_OPTION:
			options.fdupes_mode = 1;
			break;
		case DEDUPE_OPTS_OPTION:
			if (parse_dedupe_opts(optarg))
				return EINVAL;
			break;
		case 'L':
			list_only_opt = 1;
			break;
		case 'R':
			rm_only_opt = 1;
			break;
		case STATS_OPTION:
			stats_only_opt = 1;
			break;
		case QUIET_OPTION:
		case 'q':
			quiet = 1;
			break;
		case NO_COLOR_OPTION:
			opt_no_color = 1;
			break;
		case MIN_FILESIZE_OPTION:
		case 'm':
			options.min_filesize = parse_size(optarg);
			if (options.min_filesize == 0) {
				eprintf("Error: --min-filesize must be greater than zero\n");
				return EINVAL;
			}
			break;
		case EXCLUDE_OPTION:
			if (add_exclude_pattern(optarg))
				eprintf("Error: cannot exclude %s\n", optarg);
			break;
		case BATCH_SIZE_OPTION:
		case 'B':
			options.batch_size = parse_size(optarg);
			break;
		case HELP_OPTION:
			help();
			break;
		case '?':
		default:
			return 1;
		}
	}

	numfiles = argc - optind;

	if (options.only_whole_files && options.do_block_hash) {
		eprintf("Error: using both only_whole_files and partial "
			"options have no meaning\n");
		return 1;
	}

	/* Filter out option combinations that don't make sense. */
	if ((write_hashes + read_hashes + update_hashes) > 1) {
		eprintf("Error: Specify only one hashfile option.\n");
		return 1;
	}

	if (read_hashes)
		use_hashfile = H_READ;
	else if (write_hashes)
		use_hashfile = H_WRITE;
	else if (update_hashes)
		use_hashfile = H_UPDATE;

	/*
	 * Always add the hashfile and its wal etc to the exclude list
	 * A wildcard would be easier but may exclude extra files silently,
	 * this would be confusing for the user.
	 */
	if (options.hashfile != NULL) {
		char tmp[PATH_MAX + 10 ] = {0,};
		add_exclude_pattern(options.hashfile);
		snprintf(tmp, PATH_MAX + 9, "%s-wal", options.hashfile);
		add_exclude_pattern(tmp);
		snprintf(tmp, PATH_MAX + 9, "%s-shm", options.hashfile);
		add_exclude_pattern(tmp);
	}

	if (read_hashes) {
		if (numfiles) {
			eprintf("Error: --read-hashes option does not take a "
				"file list argument\n");
			return 1;
		}
		goto out_nofiles;
	}

	if (options.fdupes_mode) {
		if (read_hashes || write_hashes || update_hashes) {
			eprintf("Error: cannot mix hashfile option with "
				"--fdupes option\n");
			return 1;
		}

		if (numfiles) {
			eprintf("Error: fdupes option does not take a file "
				"list argument\n");
			return 1;
		}
		/* rest of fdupes mode is implemented in main() */
		return 0;
	}

	*filelist_idx = optind;
	if (numfiles == 1 && strcmp(argv[optind], "-") == 0)
		stdin_filelist = 1;

	if (list_only_opt + rm_only_opt + stats_only_opt > 1) {
		eprintf("Error: use only one of '-L', '-R', '--stats'.\n");
		return 1;
	}

	if (list_only_opt || rm_only_opt || stats_only_opt) {
		if (!options.hashfile || use_hashfile == H_WRITE) {
			eprintf("Error: --hashfile= option is required "
				"with '-L', '-R' or '--stats'.\n");
			return 1;
		}

		if ((list_only_opt || stats_only_opt) && numfiles) {
			eprintf("Error: -L/--stats do not take "
				"a file list argument\n");
			return 1;
		}
	}

	if (!(options.fdupes_mode || list_only_opt || stats_only_opt)
			&& numfiles == 0) {
		eprintf("Error: a file list argument is required.\n");
		return 1;
	}

out_nofiles:
	return 0;
}

static void print_header(void)
{
	vprintf("Using %uK blocks\n", blocksize / 1024);
	vprintf("Using %s hashing\n", options.do_block_hash ? "block+extent" : "extent");
#ifdef	DEBUG_BUILD
	printf("Debug build, performance may be impacted.\n");
#endif
	qprintf("%s%sScanning%s files...\n", col_bold, col_cyan, col_reset);
}

/* Process the dedupe generations in (seq_lo, seq_hi]. */
static void __process_duplicates(struct dbhandle *db, unsigned int seq_lo,
				 unsigned int seq_hi)
{
	int ret;
	struct results_tree res;
	struct hash_tree dups_tree;

	init_results_tree(&res);
	init_hash_tree(&dups_tree);

	vprintf("Loading identical files...\n");
	pdedupe_set_activity("loading identical files");
	ret = dbfile_load_same_files(db, &res, seq_lo, seq_hi);
	if (ret)
		goto out;

	if (options.run_dedupe)
		dedupe_results(&res, true);
	else
		print_dupes_table(&res, true);

	/* Reset the results_tree before loading extents or blocks */
	free_results_tree(&res);

	if (!options.only_whole_files) {
		init_results_tree(&res);

		vprintf("Loading duplicated hashes...\n");
		pdedupe_set_activity("loading duplicate extents");

		ret = dbfile_load_extent_hashes(db, &res, seq_lo, seq_hi);
		if (ret)
			goto out;

		vprintf("Found %llu identical extents\n", res.num_extents);
		if (options.do_block_hash) {
			ret = dbfile_load_block_hashes(db, &dups_tree,
						       seq_lo, seq_hi);
			if (ret)
				goto out;

			ret = find_additional_dedupe(&res);
			if (ret)
				goto out;
		}

		if (options.run_dedupe)
			dedupe_results(&res, false);
		else
			print_dupes_table(&res, false);
	}

out:
	free_results_tree(&res);
	free_hash_tree(&dups_tree);
}

/*
 * How many files' worth of dedupe generations to process per pass. The scan
 * seals a generation every --batchsize (default 1024) files, and processing
 * one generation per pass made each pass a barrier: the thread pool drained
 * at every 1024 files, so one large group regularly left every other worker
 * idle, thousands of times over on a big hashfile. Merging generations keeps
 * the pool full; the cap keeps the per-pass filerec/results memory bounded.
 */
#define DEDUPE_FILES_PER_PASS	(64 * 1024)

static void process_duplicates(struct dbhandle *db)
{
	unsigned int max = get_max_dedupe_seq(db);
	unsigned int first_seq = dedupe_seq;	/* bumped inside the loop */
	unsigned int files_per_pass = DEDUPE_FILES_PER_PASS;
	unsigned int stride, passes, pass = 0;
	/* Tests force many small passes to exercise the cross-generation path. */
	const char *env = getenv("DUPEREMOVE_FILES_PER_PASS");
	int env_val = env ? atoi(env) : 0;

	if (env_val > 0)
		files_per_pass = (unsigned int)env_val;
	stride = files_per_pass / options.batch_size;

	if (stride < 1)
		stride = 1;
	passes = max > first_seq ? (max - first_seq + stride - 1) / stride : 0;

	/*
	 * Ensure the find-dupes indexes exist. Normally built at the end of the
	 * scan; this covers read-only runs (no scan this invocation) and older
	 * hashfiles. Best-effort: the indexes only speed up the lookups below,
	 * which are correct without them, so a failure here (e.g. a read-only
	 * hashfile) is logged by the callee but must not abort the dedupe.
	 */
	(void)dbfile_create_search_indexes(db);

	/* Spawn a dedicated thread pool to block-based lookup */
	if (options.do_block_hash)
		extents_search_init();

	/* One status area + one summary spanning every batch below. Estimate
	 * the total dup-group count first so the progress and ETA have a
	 * scale. */
	if (options.run_dedupe) {
		unsigned long long total;

		if (!quiet && isatty(STDOUT_FILENO)) {
			printf("  %sAnalyzing duplicates...%s\r", col_dim, col_reset);
			fflush(stdout);
		}
		total = dbfile_count_dupe_groups(db, options.only_whole_files);
		pdedupe_begin(total, passes);
	}

	for (unsigned int i = first_seq; i < max; i += stride) {
		unsigned int hi = i + stride < max ? i + stride : max;

		if (options.run_dedupe)
			pdedupe_set_batch(++pass);

		/* Drop all filerecs from the previous iteration. Needed filerecs will be
		 * recreated by __process_duplicates()
		 */
		free_all_filerecs();
		__process_duplicates(db, i, hi);

		if (options.run_dedupe) {
			/*
			 * Bump dedupe_seq, this effectively marks the files
			 * in our hashfile as having been through dedupe.
			 */
			dedupe_seq = hi;
			dbfile_cfg.dedupe_seq = dedupe_seq;
			dbfile_cfg.blocksize = blocksize;
			dbfile_sync_config(db, &dbfile_cfg);
		}
	}

	if (options.run_dedupe)
		dedupe_end();

	if (options.do_block_hash)
		extents_search_free();
}

static int scan_files(int argc, char **argv, int filelist_idx, struct dbhandle *db)
{
	int ret;

	filescan_init();
	filescan_walk_begin();
	if (!quiet)
		pscan_run();

	/* Seed the walk with the roots (this only queues them). */
	if (stdin_filelist)
		ret = add_files_from_stdin(db);
	else
		ret = scan_files_from_cmdline(argc - filelist_idx,
					     &argv[filelist_idx], db);

	/* Run the parallel walk + scan over everything seeded above. */
	if (!ret)
		ret = filescan_walk_run(db);

	pscan_finish_listing();
	filescan_free();
	if (!quiet)
		pscan_join();

	if (ret)
		return ret;

	/*
	 * The bulk insert is done; build the find-dupes indexes now (deferred
	 * from open so scanning does not maintain them per row).
	 */
	ret = dbfile_create_search_indexes(db);
	if (ret)
		return ret;

	/*
	 * Sync the locked filesystem informations in the hashfile
	 */
	fs_get_locked_uuid(&(dbfile_cfg.fs_uuid));
	ret = dbfile_sync_config(db, &dbfile_cfg);
	if (ret)
		return ret;

	return 0;
}

int main(int argc, char **argv)
{
	int ret, filelist_idx = 0;
	_cleanup_(sqlite3_close_cleanup) struct dbhandle *db = NULL;

	char stdbuf[BUFSIZ];
	setvbuf(stdout, stdbuf, _IOLBF, BUFSIZ);

	init_filerec();

	/* Set the default CPU limits before parsing the user options */
	get_num_cpus(&(options.cpu_threads), &(options.io_threads));

	/*
	 * The detected core count can be very high (e.g. 64). Dedup is I/O
	 * bound - reading and checksumming files, then FIDEDUPERANGE ioctls -
	 * so beyond a handful of threads we mostly add lock contention and a
	 * wall of progress lines. Cap the auto-detected default; an explicit
	 * --io-threads / --cpu-threads (parsed below) still overrides it.
	 */
	if (options.io_threads > DEFAULT_MAX_AUTO_THREADS)
		options.io_threads = DEFAULT_MAX_AUTO_THREADS;
	if (options.cpu_threads > DEFAULT_MAX_AUTO_THREADS)
		options.cpu_threads = DEFAULT_MAX_AUTO_THREADS;

	ret = parse_options(argc, argv, &filelist_idx);
	if (ret) {
		exit(1);
	}

	color_init(opt_no_color);
	start_timer();

	/* Allow larger than unusal amount of open files. On linux
	 * this should bw increase form 1K to 512K open files
	 * simultaneously.
	 *
	 * On multicore SSD machines it's not hard to get to 1K open
	 * files.
	 */
	increase_limits();

	if (options.fdupes_mode)
		return process_fdupes();

	if (list_only_opt)
		return list_db_files(options.hashfile);
	else if (rm_only_opt)
		return rm_db_files(argc - filelist_idx, &argv[filelist_idx]);
	else if (stats_only_opt)
		return print_hashfile_stats(options.hashfile);

	db = dbfile_open_handle(options.hashfile);
	if (!db)
		goto out;

	dbfile_set_gdb(db);

	ret = dbfile_get_config(db->db, &dbfile_cfg);
	if (ret)
		goto out;

	dedupe_seq = dbfile_cfg.dedupe_seq;

	print_header();

	if (use_hashfile == H_WRITE || use_hashfile == H_UPDATE) {
		ret = dbfile_prune_unscanned_files(db);
		if (ret) {
			eprintf("Unable to prune unscanned files\n");
			goto out;
		}

		ret = scan_files(argc, argv, filelist_idx, db);
		if (ret)
			goto out;

		/*
		 * Drop rows for files deleted from disk since they were scanned,
		 * so a stale hashfile does not keep growing and does not make the
		 * dedupe phase load phantom groups. Before process_duplicates()
		 * so it works on the pruned set.
		 */
		{
			int64_t pruned = filescan_prune_deleted(db);

			if (pruned > 0)
				qprintf("Pruned %lld deleted file%s from the "
					"hashfile\n", (long long)pruned,
					pruned == 1 ? "" : "s");
		}

		if (options.hashfile)
			qprintf("Hashfile \"%s\" written\n",
				options.hashfile);
	}

	if (use_hashfile == H_READ || use_hashfile == H_UPDATE)
		process_duplicates(db);

	/* Reclaim space if a prune this run left the hashfile mostly free. */
	if (options.hashfile &&
	    (use_hashfile == H_WRITE || use_hashfile == H_UPDATE))
		dbfile_maybe_vacuum(db);

out:
	free_all_filerecs();

#ifdef DEBUG_BUILD
	print_mem_stats();
#else
	if (ret == ENOMEM || debug)
		print_mem_stats();
#endif

	return ret;
}
