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
#include <time.h>
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
#include "storage.h"
#include "autotune.h"

#include "opt.h"

unsigned int blocksize = DEFAULT_BLOCKSIZE;

static int stdin_filelist = 0;
static unsigned int list_only_opt = 0;
static unsigned int rm_only_opt = 0;
static unsigned int stats_only_opt = 0;
static unsigned int history_only_opt = 0;
static unsigned int json_only_opt = 0;
static unsigned int autotune_opt = 0;
static int opt_no_color = 0;

/* User-supplied --exclude patterns, captured for the self-describing hashfile
 * (the auto-added hashfile sidecar patterns are re-derived each run, not here). */
static char **user_excludes;
static int n_user_excludes;
struct dbfile_config dbfile_cfg;

static void print_file(char *filename, char *ino, char *subvol)
{
	if (verbose)
		printf("%s\t%s\t%s\n", filename, ino, subvol);
	else
		printf("%s\n", filename);
}

static int cmp_u64(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;

	return x < y ? -1 : x > y ? 1 : 0;
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
/*
 * Render a stored scan config's options as the CLI string a replay would use
 * ("-r -d --min-filesize=... --dedupe-options=..."), or "(defaults)" if none.
 * Kept next to the other scan-config policy so the flag spellings stay in one
 * place. Caller frees the result.
 */
static char *scan_config_options_str(const struct scan_config *sc)
{
	GString *s = g_string_new(NULL);
	const char *dtok[3];
	int nd = 0, j;

	if (sc->recurse)
		g_string_append(s, "-r ");
	if (sc->run_dedupe)
		g_string_append(s, "-d ");
	if (sc->skip_zeroes)
		g_string_append(s, "--skip-zeroes ");
	if (sc->min_filesize > 1)
		g_string_append_printf(s, "--min-filesize=%"PRIu64" ", sc->min_filesize);

	if (sc->only_whole_files)
		dtok[nd++] = "only_whole_files";
	if (sc->do_block_hash)
		dtok[nd++] = "partial";
	if (!sc->dedupe_same_file)
		dtok[nd++] = "nosame";
	if (nd) {
		g_string_append(s, "--dedupe-options=");
		for (j = 0; j < nd; j++)
			g_string_append_printf(s, "%s%s", j ? "," : "", dtok[j]);
	}

	if (s->len && s->str[s->len - 1] == ' ')
		g_string_truncate(s, s->len - 1);
	if (!s->len)
		g_string_append(s, "(defaults)");
	return g_string_free(s, FALSE);		/* hand the buffer to the caller */
}

struct dup_group { uint64_t size, count, waste; char *path; };

/*
 * One pass over the files table grouped by (digest, size): the whole-file
 * duplicate totals - groups, files in them, and reclaimable logical bytes.
 * When top != NULL it also fills the top `top_cap` groups by reclaimable bytes
 * (example path = the group's shortest filename: min() over a fixed-width
 * length prefix, substr() strips it) and sets *ntop. Callers wanting only the
 * totals pass top = NULL. Shared by --stats and --json.
 */
static void compute_dup_summary(sqlite3 *sq, uint64_t *groups, uint64_t *dupfiles,
				uint64_t *reclaim, struct dup_group *top,
				int top_cap, int *ntop)
{
	_cleanup_(sqlite3_stmt_cleanup) sqlite3_stmt *s = NULL;
	/* The example-path column and ordering are only needed for the top-N. */
	const char *sql = top ?
		"select size, count(*) c, (count(*)-1)*size w, "
		"substr(min(printf('%010d', length(filename)) || filename), 11) "
		"from files where digest is not null group by digest, size "
		"having c > 1 order by w desc, size desc" :
		"select size, count(*) c, (count(*)-1)*size w from files "
		"where digest is not null group by digest, size having c > 1";

	*groups = *dupfiles = *reclaim = 0;
	if (ntop)
		*ntop = 0;
	if (sqlite3_prepare_v2(sq, sql, -1, &s, NULL) != SQLITE_OK)
		return;
	while (sqlite3_step(s) == SQLITE_ROW) {
		uint64_t c = sqlite3_column_int64(s, 1);
		uint64_t w = sqlite3_column_int64(s, 2);

		(*groups)++;
		*dupfiles += c;
		*reclaim += w;
		if (top && *ntop < top_cap) {
			const char *fn = (const char *)sqlite3_column_text(s, 3);

			top[*ntop].size = sqlite3_column_int64(s, 0);
			top[*ntop].count = c;
			top[*ntop].waste = w;
			top[*ntop].path = fn ? strdup(fn) : NULL;
			(*ntop)++;
		}
	}
}

/* Format a unix timestamp as local time with strftime(3). */
static void fmt_localtime(int64_t ts, const char *fmt, char *buf, size_t n)
{
	time_t t = (time_t)ts;
	struct tm tm;

	localtime_r(&t, &tm);
	strftime(buf, n, fmt, &tm);
}

static int print_hashfile_stats(char *filename)
{
	_cleanup_(sqlite3_close_cleanup) struct dbhandle *db = NULL;
	struct dbfile_config cfg;
	struct dbfile_stats st = {0};
	struct stat sb;
	struct dup_group top[10];
	char uuid_str[37] = "";
	char htype[9] = "", b1[16], b2[16];
	int k, ntop = 0, i;
	uint64_t files = 0, hashed = 0, unhashed, logical = 0, app_id, largest = 0, median;
	uint64_t groups = 0, dupfiles = 0, reclaim = 0;
	uint64_t page_size, page_count, freelist;
	double t0, t_load, t_analysis;
	sqlite3 *sq;

	db = dbfile_open_handle_ro(filename);
	if (!db) {
		eprintf("Error: Could not open \"%s\"\n", filename);
		return -1;
	}
	sq = db->db;
	if (dbfile_get_config(sq, &cfg))
		return -1;

	/*
	 * Print each section as its data becomes ready, so on a large hashfile
	 * the identity shows instantly instead of after the whole (seconds-long)
	 * analysis. --- identity: config + a few pragmas, effectively free. ---
	 */
	uuid_unparse(cfg.fs_uuid, uuid_str);
	memcpy(htype, cfg.hash_type, 8);
	for (k = 7; k >= 0 && (htype[k] == ' ' || htype[k] == '\0'); k--)
		htype[k] = '\0';
	app_id = dbfile_query_u64(sq, "PRAGMA application_id");
	page_size = dbfile_query_u64(sq, "PRAGMA page_size");
	page_count = dbfile_query_u64(sq, "PRAGMA page_count");
	freelist = dbfile_query_u64(sq, "PRAGMA freelist_count");

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
	fflush(stdout);

	/* --- stored scan config (self-describing hashfile), if any --- */
	{
		struct scan_config sc;

		if (dbfile_load_scan_config(db, &sc) > 0) {
			char *opts = scan_config_options_str(&sc);

			printf("\n%s%sstored scan%s   %s(replayed by: oans --hashfile=%s)%s\n",
			       col_bold, col_blue, col_reset, col_dim, filename, col_reset);
			printf("  %soptions%s         %s\n", col_dim, col_reset, opts);
			free(opts);
			for (i = 0; i < sc.nroots; i++)
				printf("  %s%s%s %s\n", col_dim,
				       i == 0 ? "paths      " : "           ",
				       col_reset, sc.roots[i]);
			for (i = 0; i < sc.nexcludes; i++)
				printf("  %s%s%s %s\n", col_dim,
				       i == 0 ? "excludes   " : "           ",
				       col_reset, sc.excludes[i]);
			fflush(stdout);
			scan_config_free(&sc);
		}
	}

	/*
	 * files: one scan reads every size into an array, from which we get
	 * count/hashed/logical/largest and (after a fast C sort) the median. An
	 * in-memory qsort beats a second SQLite "order by size" over millions of
	 * rows, which is otherwise the most expensive query in the report.
	 */
	t0 = g_get_monotonic_time() / 1e6;
	dbfile_get_stats(db, &st);
	median = 0;
	files = st.num_files;
	{
		uint64_t *sizes = files ? malloc(files * sizeof(*sizes)) : NULL;
		_cleanup_(sqlite3_stmt_cleanup) sqlite3_stmt *s = NULL;
		size_t n = 0;

		if (sizes && sqlite3_prepare_v2(sq,
			"select size, digest is not null from files",
			-1, &s, NULL) == SQLITE_OK) {
			while (n < files && sqlite3_step(s) == SQLITE_ROW) {
				uint64_t sz = sqlite3_column_int64(s, 0);

				sizes[n++] = sz;
				logical += sz;
				if (sz > largest)
					largest = sz;
				if (sqlite3_column_int(s, 1))
					hashed++;
			}
			qsort(sizes, n, sizeof(*sizes), cmp_u64);
			median = n ? sizes[n / 2] : 0;
		} else {
			/* OOM: fall back to per-figure queries (median sorts in SQL). */
			hashed  = dbfile_query_u64(sq, "select count(*) from files where digest is not null");
			logical = dbfile_query_u64(sq, "select ifnull(sum(size),0) from files");
			largest = dbfile_query_u64(sq, "select ifnull(max(size),0) from files");
			median  = dbfile_query_u64(sq, "select size from files order by size "
				"limit 1 offset (select (count(*)-1)/2 from files)");
		}
		free(sizes);
	}
	unhashed = files - hashed;
	t_load = g_get_monotonic_time() / 1e6 - t0;

	printf("\n%s%sfiles%s\n", col_bold, col_blue, col_reset);
	printf("  %stracked%s         %"PRIu64"\n", col_dim, col_reset, files);
	printf("  %shashed%s          %"PRIu64"\n", col_dim, col_reset, hashed);
	if (unhashed)
		printf("  %sunread%s          %"PRIu64"   %s(unique size, whole-file mode)%s\n",
		       col_dim, col_reset, unhashed, col_dim, col_reset);
	printf("  %sextent hashes%s   %"PRIu64"\n", col_dim, col_reset, st.num_e_hashes);
	printf("  %sblock hashes%s    %"PRIu64"\n", col_dim, col_reset, st.num_b_hashes);
	printf("  %slogical data%s    %s\n", col_dim, col_reset, human_size(logical));
	printf("  %sfile sizes%s      avg %s", col_dim, col_reset,
	       human_size(files ? logical / files : 0));
	printf(" %s·%s median %s", col_dim, col_reset, human_size(median));
	printf(" %s·%s largest %s\n", col_dim, col_reset, human_size(largest));
	fflush(stdout);

	/* --- whole-file duplicates: ONE group-by feeds every figure below --- */
	t0 = g_get_monotonic_time() / 1e6;
	compute_dup_summary(sq, &groups, &dupfiles, &reclaim, top, 10, &ntop);
	t_analysis = g_get_monotonic_time() / 1e6 - t0;

	printf("\n%s%swhole-file duplicates%s\n", col_bold, col_blue, col_reset);
	printf("  %sgroups%s          %"PRIu64"\n", col_dim, col_reset, groups);
	printf("  %sfiles in groups%s %"PRIu64"\n", col_dim, col_reset, dupfiles);
	printf("  %sduplicated%s      %s%s%s   %s(%.1f%% of tracked data; logical - already"
	       " freed if deduped, see --history)%s\n",
	       col_dim, col_reset, col_green, human_size(reclaim), col_reset,
	       col_dim, logical ? 100.0 * reclaim / logical : 0.0, col_reset);
	if (ntop)
		printf("  %stop groups%s      %ssize x copies · duplicated · example%s\n",
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
	fflush(stdout);

	printf("\n%s%stiming%s\n", col_bold, col_blue, col_reset);
	printf("  %sload%s            %s\n", col_dim, col_reset,
	       fmt_dur(b1, sizeof(b1), t_load));
	printf("  %sdup analysis%s    %s   %s(the group-by over %"PRIu64" files)%s\n",
	       col_dim, col_reset, fmt_dur(b2, sizeof(b2), t_analysis),
	       col_dim, hashed, col_reset);
	return 0;
}

/* Print s as a JSON string literal (quotes + minimal escaping). */
static void print_json_str(const char *s)
{
	putchar('"');
	for (; *s; s++) {
		unsigned char c = (unsigned char)*s;

		if (c == '"' || c == '\\')
			printf("\\%c", c);
		else if (c < 0x20)
			printf("\\u%04x", c);
		else
			putchar(c);
	}
	putchar('"');
}

static int print_hashfile_history(char *filename)
{
	_cleanup_(sqlite3_close_cleanup) struct dbhandle *db = NULL;
	_cleanup_(sqlite3_stmt_cleanup) sqlite3_stmt *stmt = NULL;
	struct run_summary sum;
	char since[32];

	db = dbfile_open_handle_ro(filename);
	if (!db) {
		eprintf("Error: Could not open \"%s\"\n", filename);
		return -1;
	}
	if (dbfile_get_run_summary(db, &sum))
		return -1;

	printf("%s%soans history%s  %s\n", col_bold, col_blue, col_reset, filename);
	if (sum.runs == 0) {
		printf("  %sno runs recorded yet%s\n", col_dim, col_reset);
		return 0;
	}

	fmt_localtime(sum.first_ts, "%Y-%m-%d", since, sizeof(since));
	printf("  %ssince%s        %s  %s(%"PRIu64" run%s)%s\n", col_dim, col_reset,
	       since, col_dim, sum.runs, sum.runs == 1 ? "" : "s", col_reset);
	printf("  %sreclaimed%s    %s%s%s total\n", col_dim, col_reset, col_green,
	       human_size(sum.total_reclaimed), col_reset);
	printf("  %sfiles seen%s   %"PRIu64" %s(cumulative)%s\n", col_dim, col_reset,
	       sum.total_files, col_dim, col_reset);

	printf("\n  %srecent%s   %sdate · reclaimed · elapsed · files · mode%s\n",
	       col_dim, col_reset, col_dim, col_reset);
	if (sqlite3_prepare_v2(db->db,
		"select ts, reclaimed, duration_ms, files_scanned, deduped "
		"from run_history order by ts desc limit 20", -1, &stmt, NULL) == SQLITE_OK) {
		while (sqlite3_step(stmt) == SQLITE_ROW) {
			char when[32];

			fmt_localtime(sqlite3_column_int64(stmt, 0), "%Y-%m-%d %H:%M",
				      when, sizeof(when));
			printf("    %s  %10s  %7.1fs  %9"PRIu64"  %s\n", when,
			       human_size(sqlite3_column_int64(stmt, 1)),
			       sqlite3_column_int64(stmt, 2) / 1000.0,
			       (uint64_t)sqlite3_column_int64(stmt, 3),
			       sqlite3_column_int(stmt, 4) ? "dedupe" : "scan");
		}
	}
	return 0;
}

static int print_metrics_json(char *filename)
{
	_cleanup_(sqlite3_close_cleanup) struct dbhandle *db = NULL;
	struct dbfile_config cfg;
	struct dbfile_stats st = {0};
	struct run_summary sum;
	uint64_t logical, hashed, groups, dupfiles, reclaimable;
	sqlite3 *sq;

	db = dbfile_open_handle_ro(filename);
	if (!db) {
		eprintf("Error: Could not open \"%s\"\n", filename);
		return -1;
	}
	sq = db->db;
	if (dbfile_get_config(sq, &cfg) || dbfile_get_run_summary(db, &sum))
		return -1;
	dbfile_get_stats(db, &st);

	logical = dbfile_query_u64(sq, "select ifnull(sum(size),0) from files");
	hashed = dbfile_query_u64(sq, "select count(*) from files where digest is not null");
	compute_dup_summary(sq, &groups, &dupfiles, &reclaimable, NULL, 0, NULL);

	printf("{\n  \"hashfile\": ");
	print_json_str(filename);
	printf(",\n");
	printf("  \"format\": \"%d.%d\",\n", cfg.major, cfg.minor);
	printf("  \"block_size\": %u,\n", cfg.blocksize);
	printf("  \"files_tracked\": %"PRIu64",\n", st.num_files);
	printf("  \"files_hashed\": %"PRIu64",\n", hashed);
	printf("  \"extent_hashes\": %"PRIu64",\n", st.num_e_hashes);
	printf("  \"block_hashes\": %"PRIu64",\n", st.num_b_hashes);
	printf("  \"logical_bytes\": %"PRIu64",\n", logical);
	printf("  \"dup_groups\": %"PRIu64",\n", groups);
	printf("  \"dup_files\": %"PRIu64",\n", dupfiles);
	/* Logical upper bound; real disk freed is smaller on compressed fs. */
	printf("  \"reclaimable_logical_bytes\": %"PRIu64",\n", reclaimable);
	printf("  \"runs\": %"PRIu64",\n", sum.runs);
	printf("  \"reclaimed_total_bytes\": %"PRIu64",\n", sum.total_reclaimed);
	printf("  \"last_run_ts\": %"PRId64"\n", sum.last_ts);
	printf("}\n");
	return 0;
}

static int list_db_files(char *filename)
{
	int ret;

	_cleanup_(sqlite3_close_cleanup) struct dbhandle *db = dbfile_open_handle_ro(filename);
	if (!db) {
		eprintf("Error: Could not open \"%s\"\n", filename);
		return -1;
	}

	ret = dbfile_iter_files(db, &print_file);
	return ret;
}

/*
 * Run cb on every line read from stdin, with the trailing newline stripped.
 * Overlong lines are skipped with a warning; a nonzero cb return stops the
 * loop and is passed through.
 */
static int for_each_stdin_line(int (*cb)(char *line, void *arg), void *arg)
{
	_cleanup_(freep) char *line = NULL;
	size_t buflen = 0;
	ssize_t len;
	int ret = 0;

	while (!ret && (len = getline(&line, &buflen, stdin)) != -1) {
		if (len > 0 && line[len - 1] == '\n')
			line[--len] = '\0';

		if (len > PATH_MAX - 1) {
			eprintf("Path max exceeded: %s\n", line);
			continue;
		}

		ret = cb(line, arg);
	}
	return ret;
}

static int rm_one_path(char *path, void *db)
{
	dbfile_remove_file(db, path);
	return 0;
}

static int rm_db_files(int numfiles, char **files)
{
	int i, ret = 0;
	_cleanup_(sqlite3_close_cleanup) struct dbhandle *db = dbfile_open_handle(options.hashfile);
	if (!db) {
		eprintf("Error: Could not open \"%s\"\n", options.hashfile);
		return -1;
	}

	for (i = 0; i < numfiles; i++) {
		const char *name = files[i];

		if (strcmp(name, "-") == 0) {
			for_each_stdin_line(rm_one_path, db);
			continue;
		}

		if (dbfile_remove_file(db, name))
			ret = -1;
		else
			vprintf("Removed \"%s\" from hashfile.\n", name);
	}
	return ret;
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
	HASHFILE_OPTION,
	IO_THREADS_OPTION,
	CPU_THREADS_OPTION,
	SKIP_ZEROES_OPTION,
	DEDUPE_OPTS_OPTION,
	QUIET_OPTION,
	EXCLUDE_OPTION,
	BATCH_SIZE_OPTION,
	NO_COLOR_OPTION,
	MIN_FILESIZE_OPTION,
	STATS_OPTION,
	HISTORY_OPTION,
	JSON_OPTION,
	AUTOTUNE_OPTION,
};

static int add_one_stdin_file(char *path, void *db)
{
	if (scan_file(path, db)) {
		eprintf("Error: cannot add %s into the lookup list\n", path);
		return 1;
	}
	return 0;
}

static int add_files_from_stdin(struct dbhandle *db)
{
	return for_each_stdin_line(add_one_stdin_file, db);
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

/* One-line synopsis, printed on a usage error (e.g. run with no arguments). */
static void usage(void)
{
	eprintf("Usage: oans [options] <file|dir>...\n"
		"Try 'oans --help' for more information.\n");
}

/*
 * Full help text, printed for -h/--help. Self-contained (the man page is not
 * consulted, so this works from an uninstalled build); points at 'man 8 oans'
 * for the complete reference.
 */
static void help(void)
{
	printf(
"Usage: oans [options] <file|dir>...\n"
"\n"
"Find duplicate data across files and, with -d, ask the kernel to make them\n"
"share storage - reclaiming duplicated space without changing file contents.\n"
"Works on btrfs and xfs. Pass files/directories to scan, or a single '-' to\n"
"read the list from standard input.\n"
"\n"
"Operation:\n"
"  -d                          deduplicate: submit matching extents to the kernel\n"
"  -r                          recurse into subdirectories\n"
"      --hashfile=FILE         store hashes in FILE (SQLite) for incremental runs;\n"
"                              a bare 'oans --hashfile=FILE' replays the last run\n"
"\n"
"Scan tuning:\n"
"  -b SIZE                     hashing block size, 4K-1M (default 128K)\n"
"  -B, --batchsize=N           dedupe every N scanned files (default 1024)\n"
"  -m, --min-filesize=SIZE     skip files smaller than SIZE (default 1)\n"
"      --skip-zeroes           detect and skip all-zero blocks\n"
"      --exclude=PATTERN       exclude matching paths (may be repeated)\n"
"      --dedupe-options=OPT    [no]same, [no]partial, [no]only_whole_files\n"
"\n"
"Threads:\n"
"      --io-threads=N          threads for the I/O-bound stages (auto by default)\n"
"      --cpu-threads=N         threads for duplicate-finding (default nproc, cap 8)\n"
"\n"
"Reporting and maintenance (require --hashfile; each exits after running):\n"
"      --stats                 summarize the hashfile\n"
"      --history               show the run history and lifetime totals\n"
"      --json                  print hashfile metrics as JSON\n"
"  -L                          list files tracked in the hashfile\n"
"  -R <file>...                remove the named paths from the hashfile\n"
"      --autotune              measure the fastest --io-threads for this machine\n"
"\n"
"Other:\n"
"  -q, --quiet                 print only errors and a one-line summary\n"
"  -v                          verbose output\n"
"      --no-color              disable colored output\n"
"      --debug                 print debug messages (implies -v)\n"
"      --version               print version and exit\n"
"  -h, --help                  show this help and exit\n"
"\n"
"Full reference: man 8 oans (or docs/man/oans.md in the source tree).\n");
	exit(0);
}

/*
 * Ok this is doing more than just parsing options.
 */
static int parse_options(int argc, char **argv, int *filelist_idx)
{
	int c, numfiles;

	static struct option long_ops[] = {
		{ "debug", 0, NULL, DEBUG_OPTION },
		{ "help", 0, NULL, HELP_OPTION },
		{ "version", 0, NULL, VERSION_OPTION },
		{ "hashfile", 1, NULL, HASHFILE_OPTION },
		{ "io-threads", 1, NULL, IO_THREADS_OPTION },
		{ "cpu-threads", 1, NULL, CPU_THREADS_OPTION },
		{ "skip-zeroes", 0, NULL, SKIP_ZEROES_OPTION },
		{ "dedupe-options", 1, NULL, DEDUPE_OPTS_OPTION },
		{ "quiet", 0, NULL, QUIET_OPTION },
		{ "exclude", 1, NULL, EXCLUDE_OPTION },
		{ "batchsize", 1, NULL, BATCH_SIZE_OPTION },
		{ "no-color", 0, NULL, NO_COLOR_OPTION },
		{ "min-filesize", 1, NULL, MIN_FILESIZE_OPTION },
		{ "stats", 0, NULL, STATS_OPTION },
		{ "history", 0, NULL, HISTORY_OPTION },
		{ "json", 0, NULL, JSON_OPTION },
		{ "autotune", 0, NULL, AUTOTUNE_OPTION },
		{ NULL, 0, NULL, 0}
	};

	if (argc < 2) {
		usage();
		exit(1);
	}

	while ((c = getopt_long(argc, argv, "b:vdrh?LRqB:m:", long_ops, NULL))
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
			options.run_dedupe = 1;
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
			help(); /* Never returns */
			break;
		case HASHFILE_OPTION:
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
		case HISTORY_OPTION:
			history_only_opt = 1;
			break;
		case JSON_OPTION:
			json_only_opt = 1;
			break;
		case AUTOTUNE_OPTION:
			autotune_opt = 1;
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
			if (add_exclude_pattern(optarg)) {
				eprintf("Error: cannot exclude %s\n", optarg);
			} else {
				user_excludes = realloc(user_excludes,
					(n_user_excludes + 1) * sizeof(*user_excludes));
				abort_on(!user_excludes);
				user_excludes[n_user_excludes++] = strdup(optarg);
			}
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

	*filelist_idx = optind;
	if (numfiles == 1 && strcmp(argv[optind], "-") == 0)
		stdin_filelist = 1;

	/* -L/-R/--stats/--history/--json are exclusive read-only report modes;
	 * --autotune is exclusive with them too. */
	unsigned int report_count = list_only_opt + rm_only_opt + stats_only_opt
				  + history_only_opt + json_only_opt;
	/* Every report mode but -R takes no file list. */
	bool nofile_report = report_count && !rm_only_opt;

	if (report_count + autotune_opt > 1) {
		eprintf("Error: use only one of '-L', '-R', '--stats', "
			"'--history', '--json', '--autotune'.\n");
		return 1;
	}

	/* --autotune measures a live tree, so it needs a file list (the hashfile
	 * is optional - it is only used to persist the result). */
	if (autotune_opt && numfiles == 0) {
		eprintf("Error: --autotune needs a file or directory to "
			"measure against.\n");
		return 1;
	}

	if (report_count) {
		if (!options.hashfile) {
			eprintf("Error: --hashfile= option is required with "
				"'-L', '-R', '--stats', '--history' or '--json'.\n");
			return 1;
		}
		if (nofile_report && numfiles) {
			eprintf("Error: -L/--stats/--history/--json do not take "
				"a file list argument\n");
			return 1;
		}
	}

	/*
	 * A bare `oans --hashfile=X` (no files) is allowed: main() replays the
	 * scan config stored in the hashfile. Without a hashfile there is nothing
	 * to replay, so a file list is still required.
	 */
	if (!nofile_report && numfiles == 0 && !options.hashfile) {
		eprintf("Error: a file list argument is required.\n");
		return 1;
	}

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

/*
 * Print the scan contention diagnostics gathered this run, gated on
 * DUPEREMOVE_SCAN_STATS so a benchmark run can ask for the hard numbers without
 * the timing distortion of full -v output. empty_waits/pops says how often the
 * csum workers starved for the single producer; contended/total says how often
 * a writer had to block on the WAL write lock. Starvation-dominated means the
 * serial producer is the limiter; lock-dominated means the write lock is.
 */
static void report_scan_stats(void)
{
	uint64_t pops, empty_waits, lock_total, lock_contended, lock_wait_ns;
	uint64_t over_ns, hash_ns, cal_files, cal_bytes;

	if (!getenv("DUPEREMOVE_SCAN_STATS"))
		return;

	filescan_get_workq_stats(&pops, &empty_waits);
	dbfile_get_lock_stats(&lock_total, &lock_contended, &lock_wait_ns);

	fprintf(stderr,
		"scan-stats: csum-queue pops=%" PRIu64 " empty-waits=%" PRIu64
		" (%.1f%% starved); write-lock acquisitions=%" PRIu64
		" contended=%" PRIu64 " (%.1f%%); lock-wait total=%.2fs"
		" avg=%.1fus\n",
		pops, empty_waits,
		pops ? 100.0 * empty_waits / pops : 0.0,
		lock_total, lock_contended,
		lock_total ? 100.0 * lock_contended / lock_total : 0.0,
		lock_wait_ns / 1e9,
		lock_contended ? lock_wait_ns / 1e3 / lock_contended : 0.0);

	/*
	 * ETA calibration: the ratio of measured per-file overhead to per-byte
	 * hash time is the ideal ETA file weight for this storage; compare it to
	 * the fixed weight actually used (see progress.c). Handy for tuning the
	 * weight on real hardware - e.g. a NAS/HDD.
	 */
	filescan_get_eta_calibration(&over_ns, &hash_ns, &cal_files, &cal_bytes);
	if (cal_files && hash_ns) {
		double over_per_file = (double)over_ns / cal_files;
		double hash_per_byte = (double)hash_ns / cal_bytes;

		fprintf(stderr,
			"eta-calibration: overhead=%.1fus/file hash=%.2fs/GiB"
			" -> ideal weight %.0f KiB/file (using %" PRIu64 " KiB)\n",
			over_per_file / 1e3,
			hash_per_byte * (double)(1ULL << 30) / 1e9,
			over_per_file / hash_per_byte / 1024.0,
			pscan_eta_file_weight() / 1024);
	}
}

static int scan_files(char **roots, int nroots, struct dbhandle *db,
		      uint64_t *files_scanned)
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
		ret = scan_files_from_cmdline(nroots, roots, db);

	/* Run the parallel walk + scan over everything seeded above. */
	if (!ret)
		ret = filescan_walk_run(db);

	/*
	 * Nothing could be locked onto a supported filesystem, so the walk saw
	 * no files. Fail loudly rather than printing a misleading "Nothing to
	 * deduplicate" and exiting 0. The most common cause is XFS on a kernel
	 * older than 6.4 scanned without root (its UUID needs libblkid).
	 */
	if (!ret && filescan_seed_failed()) {
		eprintf("Error: none of the given paths are on a filesystem oans "
			"can deduplicate. Deduplication needs btrfs or XFS; for "
			"XFS on a kernel older than 6.4, run oans as root.\n");
		ret = 1;
	}

	pscan_finish_listing();
	filescan_free();
	if (!quiet)
		pscan_join();

	/*
	 * Latch the per-run file count here, while the scan owns the progress
	 * counters: the dedupe phase reuses them, so a later read would be 0.
	 */
	if (files_scanned)
		*files_scanned = pscan_files_scanned();

	report_scan_stats();

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

/* Apply a replayed scan config to the global options (see scan_config). */
static void apply_scan_config(const struct scan_config *sc)
{
	int i;

	options.run_dedupe = sc->run_dedupe;
	options.recurse_dirs = sc->recurse;
	options.skip_zeroes = sc->skip_zeroes;
	options.only_whole_files = sc->only_whole_files;
	options.do_block_hash = sc->do_block_hash;
	options.dedupe_same_file = sc->dedupe_same_file;
	options.min_filesize = sc->min_filesize;

	for (i = 0; i < sc->nexcludes; i++)
		add_exclude_pattern(sc->excludes[i]);
}

/*
 * Drop replayed roots that no longer exist (warn on each), compacting the
 * array in place. Returns the number that survive. Refusing to run when none
 * survive is the caller's job: scanning zero roots would let the stat-based
 * prune wipe the whole hashfile (e.g. an unmounted drive).
 */
static int drop_missing_roots(struct scan_config *sc)
{
	int i, live = 0;

	for (i = 0; i < sc->nroots; i++) {
		struct stat st;

		if (stat(sc->roots[i], &st) == 0) {
			sc->roots[live++] = sc->roots[i];
		} else {
			eprintf("Warning: stored path \"%s\" no longer exists, "
				"skipping.\n", sc->roots[i]);
			free(sc->roots[i]);
		}
	}
	/* Shrink the count to the compacted survivors; scan_config_free() then
	 * frees exactly those and nothing is double-freed or leaked. */
	sc->nroots = live;
	return live;
}

/*
 * Persist this run's scan-shaping options, roots and user excludes so a later
 * bare `oans --hashfile=X` replays it. Roots are canonicalised (as the scan
 * itself does) so replay is independent of the working directory. Best-effort:
 * a failure only means the next run needs its arguments again.
 */
static void persist_scan_config(struct dbhandle *db, char **roots, int nroots)
{
	struct scan_config sc = {0};
	char **abs = calloc(nroots, sizeof(*abs));
	int i;

	abort_on(!abs);
	for (i = 0; i < nroots; i++) {
		char buf[PATH_MAX];

		if (realpath(roots[i], buf))
			abs[sc.nroots++] = strdup(buf);
	}

	sc.run_dedupe = options.run_dedupe;
	sc.recurse = options.recurse_dirs;
	sc.skip_zeroes = options.skip_zeroes;
	sc.only_whole_files = options.only_whole_files;
	sc.do_block_hash = options.do_block_hash;
	sc.dedupe_same_file = options.dedupe_same_file;
	sc.min_filesize = options.min_filesize;
	sc.roots = abs;
	sc.excludes = user_excludes;
	sc.nexcludes = n_user_excludes;

	if (dbfile_store_scan_config(db, &sc))
		eprintf("Warning: could not store scan configuration in the "
			"hashfile.\n");

	for (i = 0; i < sc.nroots; i++)
		free(abs[i]);
	free(abs);
}

/*
 * Refine the auto-detected io-threads default from the backing storage of the
 * first scan target. Spinning disks are seek-bound and want fewer concurrent
 * readers than SSDs; a multi-device btrfs pool scales with its spindle count.
 * A user-supplied --io-threads is always respected. `--autotune` measures the
 * real optimum; this only picks a sensible starting point with no extra I/O.
 */
static void auto_tune_io_threads(const char *root)
{
	struct storage_profile p = {0};

	/*
	 * Detect the backing storage once: it sizes both the io-threads default
	 * and the scan-ETA per-file weight. The weight must be set even when
	 * io-threads is fixed below (an explicit flag, or a stored --autotune
	 * result), so a NAS/HDD still gets the rotational weight.
	 */
	if (root)
		storage_detect(root, &p);
	pscan_set_storage_rotational(p.rotational && p.rotational_known);

	if (options.io_threads)		/* explicit --io-threads (0 == auto) */
		return;

	/* A prior --autotune result stored in the hashfile wins over the guess. */
	if (dbfile_cfg.autotune_io_threads) {
		options.io_threads = dbfile_cfg.autotune_io_threads;
		vprintf("Using autotuned --io-threads=%u from the hashfile "
			"(override to change, re-run --autotune to update)\n",
			options.io_threads);
		return;
	}

	/*
	 * storage_recommend_io_threads() returns the plain CPU-count default for
	 * unknown media, so a failed detect (zeroed profile) still yields a sane
	 * value.
	 */
	options.io_threads = storage_recommend_io_threads(&p, get_num_cpus());

	if (verbose) {
		char desc[96];

		storage_describe(&p, desc, sizeof(desc));
		vprintf("Storage: %s; using --io-threads=%u (override to change)\n",
			desc, options.io_threads);
	}
}

int main(int argc, char **argv)
{
	int ret, filelist_idx = 0;
	int numfiles;
	char **roots;
	bool replaying = false;
	uint64_t files_scanned = 0;
	struct scan_config replay = {0};
	_cleanup_(sqlite3_close_cleanup) struct dbhandle *db = NULL;

	char stdbuf[BUFSIZ];
	setvbuf(stdout, stdbuf, _IOLBF, BUFSIZ);

	init_filerec();

	/*
	 * cpu_threads (the block-search pool) has no storage dependency, so
	 * default it here to the core count, capped: beyond a handful of threads
	 * we mostly add lock contention and a wall of progress lines. An explicit
	 * --cpu-threads (parsed below) overrides. io_threads stays 0 (== auto)
	 * and is resolved from the scan target's storage once the roots are
	 * known, in auto_tune_io_threads().
	 */
	options.cpu_threads = get_num_cpus();
	if (options.cpu_threads > AUTO_THREADS_CAP)
		options.cpu_threads = AUTO_THREADS_CAP;

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

	if (list_only_opt)
		return list_db_files(options.hashfile);
	else if (rm_only_opt)
		return rm_db_files(argc - filelist_idx, &argv[filelist_idx]);
	else if (stats_only_opt)
		return print_hashfile_stats(options.hashfile);
	else if (history_only_opt)
		return print_hashfile_history(options.hashfile);
	else if (json_only_opt)
		return print_metrics_json(options.hashfile);
	else if (autotune_opt) {
		int nroots = argc - filelist_idx;

		ret = autotune_run(&argv[filelist_idx], nroots);
		/*
		 * Record the scan config too, so autotune doubles as setup: a
		 * later bare `oans --hashfile=X` (or the systemd timer) replays
		 * these paths and options. Uses whatever -d/-r/... were passed.
		 */
		if (ret == 0 && options.hashfile && nroots > 0) {
			struct dbhandle *adb = dbfile_open_handle(options.hashfile);

			if (adb) {
				persist_scan_config(adb, &argv[filelist_idx], nroots);
				dbfile_close_handle(adb);
			}
		}
		return ret;
	}

	db = dbfile_open_handle(options.hashfile);
	if (!db)
		goto out;

	dbfile_set_gdb(db);

	ret = dbfile_get_config(db->db, &dbfile_cfg);
	if (ret)
		goto out;

	dedupe_seq = dbfile_cfg.dedupe_seq;

	numfiles = argc - filelist_idx;
	roots = &argv[filelist_idx];

	/*
	 * Self-describing hashfile: with no file arguments, replay the scan
	 * config (options + roots + excludes) the last run stored.
	 */
	if (numfiles == 0 && !stdin_filelist) {
		int have = dbfile_load_scan_config(db, &replay);

		if (have < 0) {
			ret = have;
			goto out;
		}
		if (!have) {
			eprintf("Error: no files given and the hashfile has no "
				"stored scan configuration to replay.\n"
				"Run once with the paths to record it, e.g.:\n"
				"  oans -dr --hashfile=%s <dir>...\n",
				options.hashfile);
			ret = 1;
			goto out;
		}

		apply_scan_config(&replay);
		numfiles = drop_missing_roots(&replay);
		if (numfiles == 0) {
			eprintf("Error: none of the stored paths exist; refusing "
				"to run (this would prune the whole hashfile).\n");
			ret = 1;
			goto out;
		}
		roots = replay.roots;
		replaying = true;
		qprintf("Replaying stored scan of %d path%s from the hashfile.\n",
			numfiles, numfiles == 1 ? "" : "s");
	}

	/* Pick io-threads to suit the target's storage (unless set explicitly). */
	auto_tune_io_threads(roots[0]);

	print_header();

	ret = dbfile_prune_unscanned_files(db);
	if (ret) {
		eprintf("Unable to prune unscanned files\n");
		goto out;
	}

	ret = scan_files(roots, numfiles, db, &files_scanned);
	if (ret)
		goto out;

	/* Remember this run so a later bare `oans --hashfile=X` replays it. */
	if (!replaying && !stdin_filelist && options.hashfile)
		persist_scan_config(db, roots, numfiles);

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
		qprintf("Hashfile \"%s\" written\n", options.hashfile);

	process_duplicates(db);

	/* Append this run to the hashfile's history (fuels --history / --json). */
	if (options.hashfile) {
		uint64_t groups = 0, reclaimed = 0;
		struct run_record rec;

		pdedupe_counters(&groups, &reclaimed, NULL);
		rec = (struct run_record){
			.ts = time(NULL),
			.duration_ms = (int64_t)(elapsed_seconds() * 1000.0),
			.files_scanned = files_scanned,
			.reclaimed = reclaimed,
			.groups = groups,
			.deduped = options.run_dedupe,
		};
		dbfile_record_run(db, &rec);
	}

	/* Reclaim space if a prune this run left the hashfile mostly free. */
	if (options.hashfile)
		dbfile_maybe_vacuum(db);

out:
	scan_config_free(&replay);
	free_all_filerecs();

#ifdef DEBUG_BUILD
	print_mem_stats();
#else
	if (ret == ENOMEM || debug)
		print_mem_stats();
#endif

	return ret;
}
